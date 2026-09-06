#pragma once

#include <cstdint>

namespace DekiTilemap
{

// Tiled's GID flip-flag bit layout (top 4 bits of a 32-bit GID).
constexpr uint32_t GID_FLIP_HORIZONTAL = 0x80000000u;
constexpr uint32_t GID_FLIP_VERTICAL   = 0x40000000u;
constexpr uint32_t GID_FLIP_DIAGONAL   = 0x20000000u;
constexpr uint32_t GID_ROT_HEX_120     = 0x10000000u;
constexpr uint32_t GID_FLIP_MASK       = 0xF0000000u;
constexpr uint32_t GID_INDEX_MASK      = 0x0FFFFFFFu;

inline uint32_t GidIndex(uint32_t gid) { return gid & GID_INDEX_MASK; }
inline bool GidFlipH(uint32_t gid)     { return (gid & GID_FLIP_HORIZONTAL) != 0; }
inline bool GidFlipV(uint32_t gid)     { return (gid & GID_FLIP_VERTICAL)   != 0; }
inline bool GidFlipD(uint32_t gid)     { return (gid & GID_FLIP_DIAGONAL)   != 0; }

// One resident chunk's tile data. tileGids points into a buffer owned by the
// streamer (free-list allocated, lifetime managed by LRU eviction).
struct TileChunk
{
    int32_t  chunkX;
    int32_t  chunkY;
    uint16_t layerIndex;
    uint16_t width;     // tiles
    uint16_t height;    // tiles
    uint16_t flags;     // see ChunkIndexFlags
    const uint32_t* tileGids;   // size = width * height
};

enum ChunkIndexFlags : uint16_t
{
    CHUNK_FLAG_EMPTY        = 1 << 0,  // payload is absent; treat as all-zero
    CHUNK_FLAG_UNIFORM_FILL = 1 << 1,  // payload is a single uint32 repeated for every tile
};

} // namespace DekiTilemap
