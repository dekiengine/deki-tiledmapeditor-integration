#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <deki/providers/Buffer.h>

#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/RenderPass.h"
#include "Tilemap.h"

namespace DekiTiledMap
{

class Tileset;
class TilemapComponent;

/// Registers the "tilemap" render pass. Safe to call more than once; see
/// DekiTilemapInitSystem.
void RegisterTilemapRenderPass();

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
    bool WantsObject(Deki::Object* obj) const override;
    void Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx) override;

    /// A map usually fills the view, so any change to what it draws (where it
    /// sits, its settings, a chunk loaded or dropped, a tileset arriving)
    /// redraws the whole view; a map that stays as it is redraws nothing.
    /// While chunks are still loading it counts as changed, so its Execute
    /// keeps loading them.
    bool ReportsChanges() const override { return true; }
    void AddChanges(Deki::Object* obj, const DekiRendering::RenderContext& ctx,
                    DekiRendering::DirtyRegion& changed) override;
    void FinishChanges(const DekiRendering::RenderContext& ctx, DekiRendering::DirtyRegion& changed) override;

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
        // Where the tile's opaque row spans start in TilesetCache::rowSpans,
        // or -1: with them a tile with see-through edges copies its opaque
        // run and blends only the rest, as a sprite does.
        int32_t spans;
        // Which of TilesetCache::opaqueTiles holds the tile as plain RGB565,
        // or -1. A tile with no see-through pixel in an atlas that has some
        // is drawn from its copy: copied row by row, as an opaque sprite is,
        // instead of read 3 bytes a pixel out of the atlas.
        int32_t opaque;
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
        // Each resolved tile's opaque row spans (Deki2D::BuildOpaqueRowSpans),
        // 2 per row, found once when the tile is first drawn.
        std::vector<int16_t> rowSpans;
        // The fully opaque tiles' RGB565 copies (TileLUT::opaque).
        std::vector<Deki::Buffer<uint16_t>> opaqueTiles;
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

    // AddChanges: a signature of what each map drew last, and the call that
    // last saw it.
    struct Drawn
    {
        uint64_t signature = 0;
        uint32_t seen = 0;
    };
    std::unordered_map<const TilemapComponent*, Drawn> m_Drawn;
    uint32_t m_ChangesCall = 1;

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
    // The chunk being drawn: each column's screen X and each row's screen Y.
    std::vector<int32_t> m_ChunkColX;
    std::vector<int32_t> m_ChunkRowY;
    std::vector<int> m_SrcChunkYLut;

    TilesetCache& GetCache(Tilemap* tm);
    void RefreshCache(Tilemap* tm, TilesetCache& cache);
    // A gid's (index bits only) tileset and rect in the atlas's stored pixels,
    // through the LUT when there is one. Returns false for gid 0, gids in no
    // tileset, and tilesets whose header or atlas has not loaded.
    static bool ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, int32_t& outTsIdx,
                            int32_t& outSx, int32_t& outSy, int32_t& outSw, int32_t& outSh, int32_t& outSpans,
                            int32_t& outOpaque);
};

}  // namespace DekiTiledMap
