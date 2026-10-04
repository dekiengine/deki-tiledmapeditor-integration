#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <unordered_set>

#include "TileChunk.h"
#include "Tilemap.h"
#include <deki/providers/IFileSystem.h>

namespace DekiTiledMap
{

// Loads a tilemap's chunks on demand and drops the least recently used ones
// to stay within a memory budget. Keeps one Deki::IFileSystem handle open on
// the .dtilemap. TilemapRenderSystem drives it each frame.
class TilemapStreamer
{
public:
    TilemapStreamer(Deki::IFileSystem* fs, const char* dtilemapPath, const DTilemapHeader& header,
                    const ChunkIndexEntry* index, size_t indexCount);

    ~TilemapStreamer();

    /// Marks a rect of chunks on a layer as needed; no IO yet.
    void RequestRect(int32_t layerIdx, int32_t chunkMinX, int32_t chunkMinY, int32_t chunkMaxX, int32_t chunkMaxY);

    /// Loads pending chunks, reading at most `byteBudget` bytes this call.
    void Pump(size_t byteBudget);

    /// A loaded chunk, or nullptr if it is not loaded.
    const TileChunk* Get(int32_t layerIdx, int32_t chunkX, int32_t chunkY);

    /// Marks a chunk as recently used; call it when drawing the chunk.
    void TouchLRU(int32_t layerIdx, int32_t chunkX, int32_t chunkY);

    /// Get and TouchLRU with one hash lookup. `frame` is any per-frame serial:
    /// the chunk moves in the LRU list at most once per frame, so a chunk drawn
    /// by several tilemaps (or wrapped several times) costs one list splice.
    const TileChunk* GetAndTouch(int32_t layerIdx, int32_t chunkX, int32_t chunkY, uint32_t frame);

    void SetMemoryBudget(size_t bytes);
    size_t MemoryBudget() const { return m_BudgetBytes; }
    size_t ResidentBytes() const { return m_ResidentBytes; }

    uint16_t ChunkWidth() const { return m_MHeader.chunkWidth; }
    uint16_t ChunkHeight() const { return m_MHeader.chunkHeight; }

private:
    struct Key
    {
        int32_t cx;
        int32_t cy;
        uint16_t layer;
        bool operator==(const Key& o) const { return cx == o.cx && cy == o.cy && layer == o.layer; }
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
        TileChunk chunk;
        uint32_t* owned;  // free()-able buffer behind chunk.tileGids
        size_t bytes;
        std::list<Key>::iterator lruIt;
        uint32_t lastTouchFrame = 0;  // see GetAndTouch
    };

    void EvictUntilUnder(size_t targetBytes);
    bool LoadChunkNow(const ChunkIndexEntry& entry);
    const ChunkIndexEntry* FindIndexEntry(int32_t layerIdx, int32_t cx, int32_t cy) const;

    Deki::IFileSystem* m_MFs;
    Deki::IFileSystem::FileHandle m_MHandle = nullptr;
    DTilemapHeader m_MHeader;
    const ChunkIndexEntry* m_MIndex;
    size_t m_IndexCount;

    // The last FindIndexEntry result. RequestRect walks chunks in (cy, cx)
    // order, so the next call usually wants the next entry in the sorted index;
    // this one and the one after it are tried before a binary search.
    mutable const ChunkIndexEntry* m_LastFound = nullptr;

    std::unordered_map<Key, ResidentChunk, KeyHash> m_MResident;
    std::list<Key> m_MLru;                          // back = newest
    std::list<Key> m_MPending;                      // load queue (FIFO)
    std::unordered_set<Key, KeyHash> m_PendingSet;  // fast duplicate check for m_MPending
    size_t m_ResidentBytes = 0;
    size_t m_BudgetBytes = 256 * 1024;
    size_t m_ChunkBytes = 0;
};

}  // namespace DekiTiledMap
