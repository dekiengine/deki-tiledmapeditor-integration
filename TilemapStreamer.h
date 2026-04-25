#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "TileChunk.h"
#include "Tilemap.h"
#include "providers/IDekiFileSystem.h"

namespace DekiTilemap
{

// LRU chunk pager backed by IDekiFileSystem. Holds a single open file handle
// to the .dtilemap and seeks into it to load chunks on demand. Driven by
// TilemapRenderSystem each frame.
class TilemapStreamer
{
public:
    TilemapStreamer(IDekiFileSystem* fs,
                    const char* dtilemapPath,
                    const DTilemapHeader& header,
                    const ChunkIndexEntry* index,
                    size_t indexCount);

    ~TilemapStreamer();

    // Mark a chunk-coord rect as needed on the given layer (no IO yet).
    void RequestRect(int32_t layerIdx,
                     int32_t chunkMinX, int32_t chunkMinY,
                     int32_t chunkMaxX, int32_t chunkMaxY);

    // Drain pending requests, doing at most `byteBudget` bytes of IO this call.
    void Pump(size_t byteBudget);

    // Get a resident chunk, or nullptr if not loaded yet.
    const TileChunk* Get(int32_t layerIdx, int32_t chunkX, int32_t chunkY);

    // Mark a chunk as recently used (caller does this when it draws a chunk).
    void TouchLRU(int32_t layerIdx, int32_t chunkX, int32_t chunkY);

    void   SetMemoryBudget(size_t bytes);
    size_t MemoryBudget() const { return m_budgetBytes; }
    size_t ResidentBytes() const { return m_residentBytes; }

    uint16_t ChunkWidth()  const { return m_header.chunkWidth; }
    uint16_t ChunkHeight() const { return m_header.chunkHeight; }

private:
    struct Key
    {
        int32_t  cx;
        int32_t  cy;
        uint16_t layer;
        bool operator==(const Key& o) const
        { return cx == o.cx && cy == o.cy && layer == o.layer; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        {
            uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(k.cx)) * 0x9E3779B97F4A7C15ull;
            h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.cy)) * 0xBF58476D1CE4E5B9ull;
            h ^= static_cast<uint64_t>(k.layer) * 0x94D049BB133111EBull;
            return static_cast<size_t>(h ^ (h >> 31));
        }
    };

    struct ResidentChunk
    {
        TileChunk            chunk;
        uint32_t*            owned;       // free()-able buffer behind chunk.tileGids
        size_t               bytes;
        std::list<Key>::iterator lruIt;
    };

    void EvictUntilUnder(size_t targetBytes);
    bool LoadChunkNow(const ChunkIndexEntry& entry);
    const ChunkIndexEntry* FindIndexEntry(int32_t layerIdx, int32_t cx, int32_t cy) const;

    IDekiFileSystem*             m_fs;
    IDekiFileSystem::FileHandle  m_handle = nullptr;
    DTilemapHeader               m_header;
    const ChunkIndexEntry*       m_index;
    size_t                       m_indexCount;

    std::unordered_map<Key, ResidentChunk, KeyHash> m_resident;
    std::list<Key>                                  m_lru;          // back = newest
    std::list<Key>                                  m_pending;      // load queue (FIFO)
    size_t                                          m_residentBytes = 0;
    size_t                                          m_budgetBytes   = 256 * 1024;
    size_t                                          m_chunkBytes    = 0;
};

} // namespace DekiTilemap
