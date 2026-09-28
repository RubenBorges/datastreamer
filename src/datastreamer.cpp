#include <streamingpipeline.hpp>
#include <chrono>
#include <fstream>

// Helper to construct a physical test file container structured with LZ4 chunks
void CreateDummyAssetPackage(const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary);
    
    // We will generate 3 mock chunk allocations 
    for (uint32_t i = 0; i < 3; ++i) {
        std::string rawText = "Mock structural vertex graphic data sequence for Chunk number " + std::to_string(i) + "!";
        uint32_t rawSize = rawText.size() + 1;

        // Allocate a loose bounce buffer matching LZ4 specs
        std::vector<char> compressedBuffer(LZ4_compressBound(rawSize));
        int compressedSize = LZ4_compress_default(rawText.c_str(), compressedBuffer.data(), rawSize, compressedBuffer.size());

        ChunkHeader header{};
        header.magic = 0x41535354; // "ASST"
        header.decompressedSize = rawSize;
        header.compressedSize = static_cast<uint32_t>(compressedSize);

        // Write structural chunks sequentially into target block
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(compressedBuffer.data(), compressedSize);

        // Pad file to sector boundary size rules to pass O_DIRECT hardware constraints safely
        size_t writtenSoFar = sizeof(header) + compressedSize;
        size_t remainder = 4096 - (writtenSoFar % 4096);
        if (remainder < 4096) {
            std::vector<char> padding(remainder, 0);
            file.write(padding.data(), padding.size());
        }
    }
}

int main() {
    std::filesystem::path testBundle = "EngineAssets.pack";
    std::cout << "[Main Thread] Creating dummy pack file structured layout...\n";
    CreateDummyAssetPackage(testBundle);

    StreamingPipelineManager pipeline;
    std::cout << "[Main Thread] Initializing asynchronous Direct I/O streaming systems...\n";
    if (!pipeline.InitializePipeline(testBundle)) {
        return -1;
    }

    // Simulate Multi-Draw-Indirect (MDI) GPU mapped memory pages
    constexpr size_t MOCK_BUFFER_SIZE = 1024;
    std::vector<std::byte> mockGpuVboBufferChunk0(MOCK_BUFFER_SIZE);
    std::vector<std::byte> mockGpuVboBufferChunk1(MOCK_BUFFER_SIZE);
    std::vector<std::byte> mockGpuVboBufferChunk2(MOCK_BUFFER_SIZE);

    std::cout << "[Main Thread] Scheduling 3 streaming transfers to run in parallel...\n";
    // Dispatches file offset targets calculated by sector dimensions (4KB pages matching dummy output layout)
    pipeline.QueueChunkDownload(100, 0 * 4096, mockGpuVboBufferChunk0.data());
    pipeline.QueueChunkDownload(101, 1 * 4096, mockGpuVboBufferChunk1.data());
    pipeline.QueueChunkDownload(102, 2 * 4096, mockGpuVboBufferChunk2.data());

    std::cout << "[Main Thread] Starting high-performance runtime loop...\n";
    bool running = true;
    int processedCount = 0;

    while (running) {
        // 1. Force background hardware ring queries without stalling frame ticks
        pipeline.UpdatePipeline();

        // 2. Safely harvest complete textures/meshes uploaded during the background passes
        uint32_t completedChunkID = 0;
        while (pipeline.GetReadyAssetsQueue().TryPop(completedChunkID)) {
            std::cout << "[Main Thread] SUCCESS: Ingested Asset Chunk " << completedChunkID 
                      << " into scene draw list without dropping a frame!\n";
            processedCount++;
        }

        // Break when execution context catches up with all asynchronous submissions
        if (processedCount == 3 && !pipeline.HasActiveRequests()) {
            std::cout << "[Main Thread] All scheduled async operations finalized.\n";
            running = false;
        }

        // Simulate working on game updates, math physics simulation, or renderer ticks
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS update interval
    }

    // Clean up our generated local payload
    std::filesystem::remove(testBundle);
    std::cout << "[Main Thread] Pipeline shut down cleanly.\n";
    return 0;
}
