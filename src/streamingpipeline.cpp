#include <streamingpipeline.hpp>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <lz4.h> // External compression library (Ensure you link -llz4)
#include <cstdlib>

// ==========================================
// Custom Deleter & UniqueFd Implementation
// ==========================================
void AlignedDeleter::operator()(std::byte* ptr) const noexcept {
    std::free(ptr); // posix_memalign allocations must be freed via std::free
}

void UniqueFd::reset(int new_fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

// ==========================================
// AlignedStagingPool Implementation
// ==========================================
bool AlignedStagingPool::Initialize(size_t poolSize) {
    blocks_.resize(poolSize);
    for (auto& block : blocks_) {
        block.capacityBytes = BLOCK_SIZE;
        block.isLocked = false;

        void* rawPtr = nullptr;
        if (::posix_memalign(&rawPtr, ALIGNMENT, BLOCK_SIZE) != 0) {
            std::cerr << "Critical Error: Direct I/O staging allocation failed.\n";
            return false;
        }
        block.cpuMemory.reset(static_cast<std::byte*>(rawPtr));
    }
    return true;
}

StagingBlock* AlignedStagingPool::AcquireBlock() {
    for (auto& block : blocks_) {
        if (!block.isLocked) {
            block.isLocked = true;
            return &block;
        }
    }
    return nullptr; // Pool exhausted, pipeline must await a free cycle
}

void AlignedStagingPool::ReleaseBlock(StagingBlock* block) noexcept {
    if (block) {
        block->isLocked = false;
    }
}

// ==========================================
// IOViewingLoader Implementation
// ==========================================
IOViewingLoader::IOViewingLoader() noexcept {
    std::memset(&ring_, 0, sizeof(ring_));
}

IOViewingLoader::~IOViewingLoader() {
    packageFd_.reset(); // Safely closes internal descriptor first via RAII
    if (isRingInitialized_) {
        ::io_uring_queue_exit(&ring_);
    }
}

bool IOViewingLoader::Initialize(const std::filesystem::path& packagePath) {
    if (::io_uring_queue_init(QUEUE_DEPTH, &ring_, 0) < 0) {
        std::cerr << "Failed to initialize io_uring engine layer.\n";
        return false;
    }
    isRingInitialized_ = true;

    // Open file using O_DIRECT flags to skip kernel page-caching structures
    int fd = ::open(packagePath.c_str(), O_RDONLY | O_DIRECT);
    if (fd < 0) {
        std::cerr << "Failed to open direct-access asset bundle file: " << packagePath << "\n";
        return false;
    }
    packageFd_.reset(fd);
    return true;
}

void IOViewingLoader::SubmitReadRequest(uint64_t fileOffset, StreamingRequest* request) {
    if (!packageFd_.is_valid() || !request) return;

    struct io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe) {
        std::cerr << "Submission queue bottleneck encountered!\n";
        return;
    }

    // Direct asynchronous sector read replacement for synchronized fseek/fread blocks
    ::io_uring_prep_read(sqe,
                         packageFd_.get(),
                         request->destinationBuffer,
                         request->byteSize,
                         fileOffset);

    ::io_uring_sqe_set_data(sqe, request);
    ::io_uring_submit(&ring_); // Non-blocking dispatch to kernel space
}

void IOViewingLoader::PollCompletedRequests(const std::function<void(StreamingRequest*)>& completionCallback) {
    if (!isRingInitialized_) return;

    struct io_uring_cqe* cqe = nullptr;

    // Extract out all finished CQEs without context stalling (0-timeout loop)
    while (::io_uring_peek_cqe(&ring_, &cqe) == 0) {
        if (cqe->res < 0) {
            std::cerr << "Asynchronous system I/O read failure error code: " << cqe->res << "\n";
        } else if (completionCallback) {
            auto* req = static_cast<StreamingRequest*>(::io_uring_cqe_get_data(cqe));
            if (req) {
                req->isCompleted = true;
                this->OnChunkLoaded(req);
                completionCallback(req); // Safe functional routing out to managing layers
            }
        }
        ::io_uring_cqe_seen(&ring_, cqe);
    }
}

void IOViewingLoader::OnChunkLoaded(const StreamingRequest* req) const {
    std::cout << "Chunk Context " << req->chunkID << " DMA hardware pass successfully streamed straight into memory!\n";
}

// ==========================================
// StreamingPipelineManager Implementation
// ==========================================

bool StreamingPipelineManager::InitializePipeline(const std::filesystem::path& assetPath) {
    if (!stagingPool_.Initialize(8)) return false;
    return ioLoader_.Initialize(assetPath);
}

void StreamingPipelineManager::QueueChunkDownload(uint32_t chunkID, uint64_t fileOffset, std::byte* mdiVboDst) {
    StagingBlock* block = stagingPool_.AcquireBlock();
    if (!block) {
        std::cerr << "Pipeline pipeline pipeline backing out; staging memory limit reached for Chunk " << chunkID << "\n";
        return;
    }

    auto task = std::make_unique<ChunkStreamingTask>();
    task->chunkID = chunkID;
    task->fileOffset = fileOffset;
    task->assignedStagingBlock = block;
    task->targetMdiVboPointer = mdiVboDst;

    auto ioReq = std::make_unique<StreamingRequest>();
    ioReq->chunkID = chunkID;
    ioReq->destinationBuffer = block->cpuMemory.get();
    ioReq->byteSize = block->capacityBytes;

    ioLoader_.SubmitReadRequest(fileOffset, ioReq.get());
    activeRequests_.emplace_back(std::move(ioReq), std::move(task));
}

void StreamingPipelineManager::UpdatePipeline() {
    ioLoader_.PollCompletedRequests([this](StreamingRequest* completedReq) {
        auto it = std::find_if(activeRequests_.begin(), activeRequests_.end(),
            [completedReq](const auto& pair) { return pair.first.get() == completedReq; });

        if (it != activeRequests_.end()) {
            auto task = std::move(it->second);
            activeRequests_.erase(it);
            this->OnIoReadComplete(std::move(task));
        }
    });
}

void StreamingPipelineManager::OnIoReadComplete(std::unique_ptr<ChunkStreamingTask> task) {
    std::jthread decompressionWorker([this, movedTask = std::move(task)]() mutable {
        this->ExecuteDecompression(std::move(movedTask));
    });
    decompressionWorker.detach(); 
}

void StreamingPipelineManager::ExecuteDecompression(std::unique_ptr<ChunkStreamingTask> task) {
    if (!task || !task->assignedStagingBlock) return;

    std::byte* rawStagingPtr = task->assignedStagingBlock->cpuMemory.get();
    auto* header = reinterpret_cast<ChunkHeader*>(rawStagingPtr);
    char* compressedDataStart = reinterpret_cast<char*>(rawStagingPtr) + sizeof(ChunkHeader);

    // Modern Zero-Copy Decompression directly to the mapped graphics target pointers
    int bytesDecompressed = ::LZ4_decompress_safe(
        compressedDataStart,
        reinterpret_cast<char*>(task->targetMdiVboPointer),
        header->compressedSize,
        header->decompressedSize
    );

    if (bytesDecompressed < 0) {
        std::cerr << "Data decompression task corruption error inside Chunk ID: " << task->chunkID << "\n";
    }

    // Relinquish buffer lease back to pool immediately for active re-use by hardware ring
    stagingPool_.ReleaseBlock(task->assignedStagingBlock);

    this->NotifyMainThreadAssetReady(task->chunkID);
}

void StreamingPipelineManager::NotifyMainThreadAssetReady(uint32_t chunkID) {
    // Inter-thread boundary crossing via synchronized shared concurrency queue
    readyAssetsQueue_.Push(chunkID);
}