#pragma once

#include <queue>       // 1. Added to fix the std::queue errors
#include <mutex>       // (Ensure you have these for your ConcurrentQueue)
#include <condition_variable>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <filesystem>
#include <cstdint>
#include <cstddef>
#include <thread>
#include <functional>
#include <liburing.h> // Ensure you link -luring
#include <lz4.h>

#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

// ==========================================
// Thread-Safe Concurrent Queue for Main Thread Sync
// ==========================================
template <typename T>
class ConcurrentQueue {
public:
    void Push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
    }

    // Non-blocking try-pop designed for real-time engine frame loops
    bool TryPop(T& outValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        outValue = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool IsEmpty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
};

// Custom deleter helper for posix_memalign allocations
struct AlignedDeleter {
    void operator()(std::byte* ptr) const noexcept;
};

// RAII Wrapper for POSIX file descriptors
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        int old_fd = fd_;
        fd_ = -1;
        return old_fd;
    }

    void reset(int new_fd = -1) noexcept;

private:
    int fd_ = -1;
};

// Metadata tracking what asset an io_uring request belongs to
struct StreamingRequest {
    uint32_t chunkID = 0;
    std::byte* destinationBuffer = nullptr;
    size_t byteSize = 0;
    bool isCompleted = false;
};

// Represents a memory block aligned to sector boundaries
struct StagingBlock {
    std::unique_ptr<std::byte[], AlignedDeleter> cpuMemory = nullptr;
    size_t capacityBytes = 0;
    bool isLocked = false;
};

// Fixed pool managing sector-aligned I/O staging blocks
class AlignedStagingPool {
public:
    AlignedStagingPool() = default;
    ~AlignedStagingPool() = default;

    // Disallow copies due to unique resource ownership
    AlignedStagingPool(const AlignedStagingPool&) = delete;
    AlignedStagingPool& operator=(const AlignedStagingPool&) = delete;
    AlignedStagingPool(AlignedStagingPool&&) noexcept = default;
    AlignedStagingPool& operator=(AlignedStagingPool&&) noexcept = default;

    bool Initialize(size_t poolSize = 8);
    [[nodiscard]] StagingBlock* AcquireBlock();
    void ReleaseBlock(StagingBlock* block) noexcept;

private:
    std::vector<StagingBlock> blocks_;
    static constexpr size_t ALIGNMENT = 4096;       // 4KB required by O_DIRECT
    static constexpr size_t BLOCK_SIZE = 2 * 1024 * 1024; // 2MB chunk capacity
};

// Asynchronous hardware DMA storage loader powered by io_uring
class IOViewingLoader {
public:
    IOViewingLoader() noexcept;
    ~IOViewingLoader();

    IOViewingLoader(const IOViewingLoader&) = delete;
    IOViewingLoader& operator=(const IOViewingLoader&) = delete;
    IOViewingLoader(IOViewingLoader&&) noexcept = delete; // io_uring internal pointers restrict raw moves
    IOViewingLoader& operator=(IOViewingLoader&&) noexcept = delete;

    bool Initialize(const std::filesystem::path& packagePath);
    void SubmitReadRequest(uint64_t fileOffset, StreamingRequest* request);
    void PollCompletedRequests(const std::function<void(StreamingRequest*)>& completionCallback);

private:
    void OnChunkLoaded(const StreamingRequest* req) const;

    struct io_uring ring_;
    UniqueFd packageFd_;
    bool isRingInitialized_ = false;
    static constexpr size_t QUEUE_DEPTH = 256;
};

// High-level structure tracking cross-thread asynchronous operations
struct ChunkStreamingTask {
    uint32_t chunkID = 0;
    uint64_t fileOffset = 0;
    StagingBlock* assignedStagingBlock = nullptr;
    std::byte* targetMdiVboPointer = nullptr;
};

struct ChunkHeader {
    uint32_t magic;
    uint32_t decompressedSize;
    uint32_t compressedSize;
};

class StreamingPipelineManager {
public:
    StreamingPipelineManager() = default;
    ~StreamingPipelineManager() = default;

    StreamingPipelineManager(const StreamingPipelineManager&) = delete;
    StreamingPipelineManager& operator=(const StreamingPipelineManager&) = delete;

    bool InitializePipeline(const std::filesystem::path& assetPath);
    void QueueChunkDownload(uint32_t chunkID, uint64_t fileOffset, std::byte* mdiVboDst);
    void UpdatePipeline();

    // Accessor for the main thread update loop to harvest completed visual asset IDs
    ConcurrentQueue<uint32_t>& GetReadyAssetsQueue() { return readyAssetsQueue_; }
    [[nodiscard]] bool HasActiveRequests() const { return !activeRequests_.empty(); }

private:
    void OnIoReadComplete(std::unique_ptr<ChunkStreamingTask> task);
    void ExecuteDecompression(std::unique_ptr<ChunkStreamingTask> task);
    void NotifyMainThreadAssetReady(uint32_t chunkID);

    AlignedStagingPool stagingPool_;
    IOViewingLoader ioLoader_;
    
    // Tracks outstanding raw requests tied to unique tasks inside the active io_uring ring
    std::vector<std::pair<std::unique_ptr<StreamingRequest>, std::unique_ptr<ChunkStreamingTask>>> activeRequests_;
    
    // Thread-safe dispatch queue targeting main-thread frame ingestion routines
    ConcurrentQueue<uint32_t> readyAssetsQueue_;
};
