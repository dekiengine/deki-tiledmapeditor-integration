#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/RenderPass.h"
#include "Tilemap.h"

namespace DekiTiledMap
{

class Tileset;

// Per-object render pass that draws every TilemapComponent it sees.
//
// For each visible chunk on each enabled layer, draws one BlitScaled per
// non-empty tile from the tileset atlas. Feeds the chunk streamer the
// camera's visible rect (plus chunkPadding) and a per-frame IO budget.
//
// Registered with DekiRendering::DekiRenderPassRegistry as "tilemap", so the
// project's .rpipeline can turn it on.
class TilemapRenderPass : public DekiRendering::RenderPass
{
public:
    static constexpr const char* kRegistryName = "tilemap";

    /// Execute only: the renderer skips this pass for the other four hooks.
    uint32_t HookMask() const override { return DekiRendering::RenderPassHooks::Execute; }
    void Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx) override;

private:
    // Where a global tile id lives: its tileset index and pixel rect in that
    // tileset's atlas. One indexed load instead of a binary search over the
    // tilesets and two divisions per tile per frame.
    struct TileLUT
    {
        int32_t tsIdx;  // >= 0 found; kUnmapped; kUnresolved (filled on first use)
        int32_t sx;     // the tile's rect in the atlas's stored pixels (fewer
        int32_t sy;     // than the tileset's when Max Size shrank the atlas)
        int32_t sw;
        int32_t sh;
    };
    static constexpr int32_t kUnmapped = -1;
    static constexpr int32_t kUnresolved = -2;

    // What a Tilemap's tilesets resolve to, built on first use. Entries whose
    // atlas has not loaded are tried again each frame. Tileset and atlas pixel
    // pointers are checked each frame, so hot reload and async loads refresh
    // on their own.
    struct TilesetCache
    {
        std::vector<Tileset*> tilesets;
        std::vector<QuadBlit::Source> sources;  // base atlas + chroma key (no per-tile data)
        std::vector<bool> ready;
        // Per tileset, set each frame: the drawn tile size at this camera
        // scale, and a Source each tile changes in place (pixels, flips)
        // instead of copying the 72-byte base per tile.
        std::vector<int32_t> destW;
        std::vector<int32_t> destH;
        std::vector<QuadBlit::Source> scratch;
        // Gid lookup table, sized to the highest gid any tileset covers and
        // filled on use. gidLimit is 0 until every tileset header is loaded.
        std::vector<TileLUT> gidLut;
        uint32_t gidLimit = 0;
        // The AssetManager epoch the sources were resolved in. When the global
        // epoch moves (UnloadAll, InvalidateAsset, hot reload), sources[].pixels
        // may point into freed atlas memory, so RefreshCache resolves them all
        // again.
        uint64_t epoch = 0;
    };
    std::vector<std::pair<Tilemap*, TilesetCache>> m_MCaches;
    // The epoch m_MCaches was built in. The Tilemap* keys are asset pointers,
    // so when the AssetManager epoch moves every entry is dropped.
    uint64_t m_CachesEpoch = 0;
    // Bumped per Execute, so the streamer moves a chunk's LRU node at most once
    // per frame however often the chunk is drawn.
    uint32_t m_FrameSerial = 0;

    // Scratch kept between calls, so Execute() does not allocate.
    std::vector<ChunkIndexEntry> m_VisibleScratch;
    struct VisChunk
    {
        int drawX;
        int drawY;
        int srcX;
        int srcY;
    };
    std::vector<VisChunk> m_DrawsScratch;
    std::vector<int> m_SrcChunkXLut;
    std::vector<int> m_SrcChunkYLut;

    TilesetCache& GetCache(Tilemap* tm);
    void RefreshCache(Tilemap* tm, TilesetCache& cache);
    // A gid's (index bits only) tileset and rect in the atlas's stored pixels,
    // through the LUT when there is one. Returns false for gid 0, gids in no
    // tileset, and tilesets whose header or atlas has not loaded.
    static bool ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, int32_t& outTsIdx,
                            int32_t& outSx, int32_t& outSy, int32_t& outSw, int32_t& outSh);
};

}  // namespace DekiTiledMap
