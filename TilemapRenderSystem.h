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
// the camera's visible rect (plus chunk_padding) and a per-frame IO budget.
//
// Registered with DekiRenderPassRegistry under the name "tilemap" so the
// project's .rpipeline can activate it.
class TilemapRenderPass : public RenderPass
{
public:
    static constexpr const char* RegistryName = "tilemap";

    void Execute(DekiObject* obj, RenderContext& ctx) override;

private:
    // Per-Tilemap resolution cache. Built lazily on first frame; entries with
    // a not-yet-loaded atlas are retried each frame until ready. Tileset asset
    // pointers and atlas pixel pointers are validated each frame so hot-reload
    // and async load completion both refresh transparently.
    struct TilesetCache
    {
        std::vector<Tileset*>          tilesets;
        std::vector<QuadBlit::Source>  sources;  // base atlas + chroma key (no per-tile data)
        std::vector<bool>              ready;
        // AssetManager epoch the source pointers were resolved against. When
        // the global epoch advances (UnloadAll / InvalidateAsset / hot-reload),
        // sources[].pixels can dangle into freed atlas memory; bumping triggers
        // a full re-resolve in RefreshCache.
        uint64_t                       epoch = 0;
    };
    std::vector<std::pair<Tilemap*, TilesetCache>> m_caches;

    // Reused scratch — avoids heap traffic in Execute().
    std::vector<ChunkIndexEntry> m_visibleScratch;
    struct VisChunk { int drawX; int drawY; int srcX; int srcY; };
    std::vector<VisChunk>        m_drawsScratch;
    std::vector<int>             m_srcChunkXLut;
    std::vector<int>             m_srcChunkYLut;

    TilesetCache& GetCache(Tilemap* tm);
    void          RefreshCache(Tilemap* tm, TilesetCache& cache);
};

} // namespace DekiTilemap
