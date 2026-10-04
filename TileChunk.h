#pragma once

#include <cstdint>

namespace DekiTiledMap
{

// Tiled's GID flip-flag bit layout (top 4 bits of a 32-bit GID).
constexpr uint32_t kGidFlipHorizontal = 0x80000000u;
constexpr uint32_t kGidFlipVertical = 0x40000000u;
constexpr uint32_t kGidFlipDiagonal = 0x20000000u;
constexpr uint32_t kGidRotHex120 = 0x10000000u;
constexpr uint32_t kGidFlipMask = 0xF0000000u;
constexpr uint32_t kGidIndexMask = 0x0FFFFFFFu;

inline uint32_t GidIndex(uint32_t gid)
{
    return gid & kGidIndexMask;
}
inline bool GidFlipH(uint32_t gid)
{
    return (gid & kGidFlipHorizontal) != 0;
}
inline bool GidFlipV(uint32_t gid)
{
    return (gid & kGidFlipVertical) != 0;
}
inline bool GidFlipD(uint32_t gid)
{
    return (gid & kGidFlipDiagonal) != 0;
}

// One resident chunk's tile data. tileGids points into a buffer owned by the
// streamer (free-list allocated, lifetime managed by LRU eviction).
struct TileChunk
{
    int32_t chunkX;
    int32_t chunkY;
    uint16_t layerIndex;
    uint16_t width;            // tiles
    uint16_t height;           // tiles
    uint16_t flags;            // see ChunkIndexFlags
    const uint32_t* tileGids;  // size = width * height
};

enum ChunkIndexFlags : uint16_t
{
    ChunkFlagEmpty = 1 << 0,        // payload is absent; treat as all-zero
    ChunkFlagUniformFill = 1 << 1,  // payload is a single uint32 repeated for every tile
};

}  // namespace DekiTiledMap
