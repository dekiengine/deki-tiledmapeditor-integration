#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/RenderPass.h"
#include "Tilemap.h"

namespace DekiTilemap
{

class Tileset;

// Per-object render pass that draws every TilemapComponent it sees.
//
// For each visible chunk on each enabled layer, emits one BlitScaled per
// non-zero tile against the tileset atlas. Drives the chunk streamer with
// the camera's visible rect (plus chunkPadding) and a per-frame IO budget.
//
// Registered with DekiRenderPassRegistry under the name "tilemap" so the
// project's .rpipeline can activate it.
class TilemapRenderPass : public RenderPass
{
public:
    static constexpr const char* RegistryName = "tilemap";

    // Execute only: the renderer skips this pass for the other four hooks.
    uint32_t HookMask() const override { return RenderPassHooks::Execute; }
    void Execute(DekiObject* obj, RenderContext& ctx) override;

private:
    // Per-Tilemap resolution cache. Built lazily on first frame; entries with
    // a not-yet-loaded atlas are retried each frame until ready. Tileset asset
    // pointers and atlas pixel pointers are validated each frame so hot-reload
    // and async load completion both refresh transparently.
    // Where a global tile id lives: tileset index and its pixel rect origin in
    // that tileset's atlas. Replaces a binary search over the tilesets plus two
    // divisions per tile per frame with one indexed load.
    struct TileLUT
    {
        int32_t tsIdx;  // >= 0 resolved; kUnmapped; kUnresolved (fill on first use)
        int32_t sx;
        int32_t sy;
    };
    static constexpr int32_t kUnmapped = -1;
    static constexpr int32_t kUnresolved = -2;

    struct TilesetCache
    {
        std::vector<Tileset*>          tilesets;
        std::vector<QuadBlit::Source>  sources;  // base atlas + chroma key (no per-tile data)
        std::vector<bool>              ready;
        // Per-frame per-tileset values: destination tile size at this camera
        // scale, and the Source each tile mutates in place (pixels + flips)
        // instead of copying the 72-byte base per tile.
        std::vector<int32_t>           destW;
        std::vector<int32_t>           destH;
        std::vector<QuadBlit::Source>  scratch;
        // Gid lookup table, sized to the highest gid any tileset covers and
        // filled lazily. gidLimit == 0 until every tileset header is loaded.
        std::vector<TileLUT>           gidLut;
        uint32_t                       gidLimit = 0;
        // AssetManager epoch the source pointers were resolved against. When
        // the global epoch advances (UnloadAll / InvalidateAsset / hot-reload),
        // sources[].pixels can dangle into freed atlas memory; bumping triggers
        // a full re-resolve in RefreshCache.
        uint64_t                       epoch = 0;
    };
    std::vector<std::pair<Tilemap*, TilesetCache>> m_MCaches;
    // Epoch m_MCaches was built under. The Tilemap* keys are asset pointers, so
    // when the AssetManager epoch moves every entry is dropped, not refreshed.
    uint64_t m_cachesEpoch = 0;
    // Bumped per Execute; lets the streamer relink a chunk's LRU node at most
    // once per frame however many times the chunk is drawn.
    uint32_t m_frameSerial = 0;

    // Reused scratch — avoids heap traffic in Execute().
    std::vector<ChunkIndexEntry> m_visibleScratch;
    struct VisChunk { int drawX; int drawY; int srcX; int srcY; };
    std::vector<VisChunk>        m_drawsScratch;
    std::vector<int>             m_srcChunkXLut;
    std::vector<int>             m_srcChunkYLut;

    TilesetCache& GetCache(Tilemap* tm);
    void          RefreshCache(Tilemap* tm, TilesetCache& cache);
    // Resolve a gid (index bits only) to its tileset + atlas origin, through
    // the LUT when it exists. Returns false for gid 0, unmapped gids and
    // tilesets whose header has not loaded yet.
    static bool   ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex,
                              int32_t& outTsIdx, int32_t& outSx, int32_t& outSy);
};

} // namespace DekiTilemap
