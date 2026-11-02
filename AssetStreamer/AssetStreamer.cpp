#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>

#pragma pack(push, 1)

// Individual instance of a pre-baked asset placed in the world
struct PreBakedInstance {
    uint32_t assetID;       // Maps back to Global Mesh Registry
    float transform[16];    // 4x4 Matrix for position, rotation, scale
};

// Layout of a data chunk stored on disk
struct ChunkPayloadHeader {
    uint32_t seed;               // Deterministic seed for procedural generation
    uint32_t preBakedCount;      // Number of explicit items in this chunk
    // Followed immediately by: PreBakedInstance instances[preBakedCount]
};

// Global File Header
struct ArchiveHeader {
    char magic[4];               // 'PACK'
    uint32_t version;            // e.g., 1
    uint32_t globalMeshCount;    // How many distinct 3D models are embedded
    uint32_t gridWidth;          // Width of the world grid
    uint32_t gridHeight;         // Height of the world grid
};

#pragma pack(pop)

class AssetStreamer {
private:
    int fileDescriptor = -1;
    size_t fileSize = 0;
    uint8_t* fileData = nullptr; // Memory-mapped base pointer

    ArchiveHeader* header = nullptr;
    uint64_t* gridOffsets = nullptr;

public:
    bool loadPackFile(const char* filePath) {
        fileDescriptor = open(filePath, O_RDONLY);
        if (fileDescriptor < 0) return false;

        struct stat sb;
        if (fstat(fileDescriptor, &sb) == -1) return false;
        fileSize = sb.st_size;

        // Map the entire file into virtual memory addresses
        fileData = (uint8_t*)mmap(nullptr, fileSize, PROT_READ, MAP_SHARED, fileDescriptor, 0);
        if (fileData == MAP_FAILED) return false;

        // Parse root pointers instantly (Zero-Copy)
        header = reinterpret_cast<ArchiveHeader*>(fileData);
        
        // Grid Table sits right after the header + mesh registry
        size_t registrySize = header->globalMeshCount * (sizeof(uint64_t) * 2);
        gridOffsets = reinterpret_cast<uint64_t*>(fileData + sizeof(ArchiveHeader) + registrySize);

        return true;
    }

    // O(1) Seek & Load Chunk Data
    void streamChunk(int32_t chunkX, int32_t chunkZ) {
        // Simple coordinate wrapping for infinite looping in your toy example
        uint32_t wrappedX = static_cast<uint32_t>(chunkX % header->gridWidth);
        uint32_t wrappedZ = static_cast<uint32_t>(chunkZ % header->gridHeight);
        uint64_t linearIndex = wrappedX + (wrappedZ * header->gridWidth);

        uint64_t chunkOffset = gridOffsets[linearIndex];
        if (chunkOffset == 0) return; // Empty region

        // Step 1: Jump directly to the payload pointer
        ChunkPayloadHeader* chunk = reinterpret_cast<ChunkPayloadHeader*>(fileData + chunkOffset);

        // Step 2: Feed the deterministic seed to your procedural engine (Trees/Grass)
        generateProceduralAssets(chunk->seed, chunkX, chunkZ);

        // Step 3: Extract pre-baked unique objects (Hero assets/Town structures)
        if (chunk->preBakedCount > 0) {
            PreBakedInstance* instances = reinterpret_cast<PreBakedInstance*>(fileData + chunkOffset + sizeof(ChunkPayloadHeader));
            
            // Fast Upload: Pass the 'instances' raw array pointer straight 
            // to a GPU Instanced Vertex Buffer. No allocations on CPU!
            uploadToGPUVertexBuffer(instances, chunk->preBakedCount);
        }
    }

    ~AssetStreamer() {
        if (fileData && fileData != MAP_FAILED) munmap(fileData, fileSize);
        if (fileDescriptor >= 0) close(fileDescriptor);
    }

private:
    void generateProceduralAssets(uint32_t seed, int32_t cx, int32_t cz) {
        // Feed seed to a fast randomizer (like Squirrel3 or PCG hash)
        // Scatter low-priority items deterministically based on player grid
    }

    void uploadToGPUVertexBuffer(PreBakedInstance* data, uint32_t count) {
        // Engine/API specific upload step
    }
};
