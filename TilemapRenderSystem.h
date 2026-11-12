#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

#include <deki/providers/Buffer.h>
#include <deki/providers/Rect.h>

#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/FrameCamera.h"
#include "deki-rendering/RenderPass.h"
#include "Tilemap.h"
#include "TilemapStreamer.h"

namespace DekiTiledMap
{

// A growable array of plain values in external memory (PSRAM on a board):
// per-tile tables that grow with every tile drawn stay out of the internal RAM
// a board's display and frame need. Grown by doubling, contents kept.
template <typename T>
class ExternalArray
{
public:
    size_t size() const { return m_Size; }
    T* data() { return m_Buffer.Data(); }
    const T* data() const { return m_Buffer.Data(); }
    void clear() { m_Size = 0; }
    // New elements are zero. False (and the size unchanged) when there is no
    // memory for it.
    bool resize(size_t count)
    {
        if (count > m_Buffer.Count())
        {
            Deki::Buffer<T> grown;
            if (!grown.Allocate(std::max(count, m_Buffer.Count() * 2), Deki::Memory::External))
            {
                return false;
            }
            if (m_Size > 0)
            {
                std::memcpy(grown.Data(), m_Buffer.Data(), m_Size * sizeof(T));
            }
            m_Buffer = std::move(grown);
        }
        if (count > m_Size)
        {
            std::memset(m_Buffer.Data() + m_Size, 0, (count - m_Size) * sizeof(T));
        }
        m_Size = count;
        return true;
    }

private:
    Deki::Buffer<T> m_Buffer;
    size_t m_Size = 0;
};

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
    const char* Name() const override { return "Tilemap"; }
    /// Every tile is a QuadBlit blit.
    bool WritesBuffer() const override { return false; }
    bool FollowsScroll() const override { return true; }
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
        // Where a tile's image goes from its cell's bottom-left, in screen
        // pixels: the tileset's Tile Offset and Tile Render Size, scaled.
        std::vector<int32_t> cellLeft;
        std::vector<int32_t> cellTop;
        std::vector<QuadBlit::Source> scratch;
        // Gid lookup table, sized to the highest gid any tileset covers and
        // filled on use. gidLimit is 0 until every tileset header is loaded.
        std::vector<TileLUT> gidLut;
        // Each resolved tile's opaque row spans (Deki2D::BuildOpaqueRowSpans),
        // 2 per row, found once when the tile is first drawn. At the same
        // place in clearSpans, its transparent runs (BuildClearRowSpans),
        // and at half that place in uniformRuns, its runs of one colour
        // (QuadBlit::FindUniformRuns): a blit skips the first unread and
        // fills the second, as a sprite's.
        // In external memory: they grow with every tile drawn.
        ExternalArray<int16_t> rowSpans;
        ExternalArray<int16_t> clearSpans;
        ExternalArray<QuadBlit::UniformRun> uniformRuns;
        // The fully opaque tiles' RGB565 copies (TileLUT::opaque), and a
        // Source::revision for each: made once, they never change.
        std::vector<Deki::Buffer<uint16_t>> opaqueTiles;
        std::vector<uint32_t> opaqueRevisions;
        uint32_t gidLimit = 0;
        // Tile animations, found as tiles are drawn: per gid, -2 not looked
        // at yet, -1 not animated, else an index into anims.
        struct TileAnimation
        {
            std::vector<uint32_t> gids;    // each frame's tile
            std::vector<uint32_t> endsMs;  // when each frame ends, from the start
            uint32_t totalMs = 0;
        };
        std::vector<int32_t> animOf;
        std::vector<TileAnimation> anims;
        // A box of map pixels (Y down) that holds every tile the map can
        // draw: its chunks, grown by its largest tile image. Worked out again
        // when another tileset is ready (`boxReady`).
        bool boxKnown = false;
        uint32_t boxReady = 0;
        float boxLeft = 0.0f, boxTop = 0.0f, boxRight = 0.0f, boxBottom = 0.0f;
        // Per tileset, whether every pixel of its atlas is fully see-through
        // or fully opaque (0 not looked at yet, 1 yes, 2 no): layers of such
        // tiles look the same drawn one over another as their copy does.
        std::vector<uint8_t> binaryAlpha;
        // The AssetManager epoch the sources were resolved in. When the global
        // epoch moves (UnloadAll, InvalidateAsset, hot reload), sources[].pixels
        // may point into freed atlas memory, so RefreshCache resolves them all
        // again.
        uint64_t epoch = 0;
        // The tilesets each chunk's tiles come from, a bit each (all bits for
        // a 33rd and on, or while one has not loaded), so a layer walks only
        // the cells its own tiles can reach the clip from: a map's tall
        // building tiles do not widen the walk of its ground. Kept until the
        // streamer loads or drops a chunk (`stamp`).
        struct ChunkTilesets
        {
            const TileChunk* chunk;
            uint32_t mask;
        };
        ExternalArray<ChunkTilesets> chunkTilesets;
        uint32_t chunkTilesetsStamp = 0;
    };
    // The tilesets `chunk` uses (TilesetCache::chunkTilesets).
    static uint32_t TilesetsOf(const Tilemap* tm, TilesetCache& cache, const TileChunk* chunk, uint32_t stamp);
    std::vector<std::pair<Tilemap*, TilesetCache>> m_MCaches;
    // The epoch m_MCaches was built in. The Tilemap* keys are asset pointers,
    // so when the AssetManager epoch moves every entry is dropped.
    uint64_t m_CachesEpoch = 0;
    // Bumped per Execute, so the streamer moves a chunk's LRU node at most once
    // per frame however often the chunk is drawn.
    uint32_t m_FrameSerial = 0;
    // Chunks asked for in this frame, by map, layers, shift and clip: a part
    // of the frame drawn later within a clip already asked for asks again for
    // nothing (RenderContext::contentFrame names the frame).
    struct Requested
    {
        const TilemapComponent* tc;
        uint32_t layers;
        float shiftX, shiftY;
        uint32_t frame;
        int32_t left, top, right, bottom;
    };
    ExternalArray<Requested> m_Requested;

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

    // Screen positions of a grid's columns or rows, each worked out at most
    // once a DrawLayers call however many layers ask: they depend on the
    // camera and the map, not the layer. A position outside the range set is
    // worked out every time it is asked for.
    struct AxisCache
    {
        int32_t first = 0;
        int32_t count = 0;
        ExternalArray<int32_t> values;
        ExternalArray<uint8_t> known;
        void Reset(int32_t lo, int32_t hi)
        {
            first = lo;
            count = hi >= lo && known.resize(static_cast<size_t>(hi - lo + 1)) &&
                            values.resize(static_cast<size_t>(hi - lo + 1))
                        ? hi - lo + 1
                        : 0;
            if (count > 0)
            {
                std::memset(known.data(), 0, static_cast<size_t>(count));
            }
        }
        template <typename Compute>
        int32_t Get(int32_t i, Compute compute)
        {
            const int32_t at = i - first;
            if (at < 0 || at >= count)
            {
                return compute(i);
            }
            if (!known.data()[at])
            {
                values.data()[at] = compute(i);
                known.data()[at] = 1;
            }
            return values.data()[at];
        }
    };
    AxisCache m_AxisCache[4];

    // The chunks of an isometric layer in view this frame.
    struct IsoChunk
    {
        const TileChunk* chunk;
        int baseColumn;
        int baseRow;
    };
    std::vector<IsoChunk> m_IsoChunks;
    std::vector<int> m_SrcChunkYLut;

    // A layer with a blend mode: each tile is drawn on black and on white
    // (which gives its colours and its alpha), then blended into the frame.
    Deki::Buffer<uint32_t> m_BlendOnBlack;
    Deki::Buffer<uint32_t> m_BlendOnWhite;
    // A see-through tile on an RGB565 frame is drawn once instead, on clear
    // RGB565A8, which keeps its alpha: its colours are 565 anyway.
    Deki::Buffer<uint8_t> m_Blend565;
    std::vector<uint32_t> m_BlendRow;    // a row of the frame being blended into
    std::vector<uint32_t> m_BlendDrawn;  // a row of the drawn colours, times alpha
    std::vector<uint8_t> m_BlendAlpha;   // and their alphas
    // `coversRect`: the source fills its rect with opaque pixels (no alpha, no
    // colour key), so with an opaque tint it is drawn once, on nothing.
    void BlendBlit(const QuadBlit::Source& source, DekiRendering::RenderContext& ctx, int destX, int destY, int destW,
                   int destH, uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA, uint8_t blendMode,
                   bool coversRect);

    TilesetCache& GetCache(Tilemap* tm);

    // Draws the tile layers in layerMask, moved by (shiftX, shiftY) meters and
    // with a layer's tint (RGBA8, R low) and opacity on top of the component's.
    void DrawLayers(Deki::Object* obj, DekiRendering::RenderContext& ctx, uint32_t layerMask, float shiftX,
                    float shiftY, uint32_t layerTint, float layerOpacity, uint8_t blendMode = 0);

    // Draws an image layer where Tiled puts it, moved by (shiftX, shiftY) meters.
    void DrawImageLayer(Deki::Object* obj, DekiRendering::RenderContext& ctx, const DLayerLook& look, float shiftX,
                        float shiftY);
    void RefreshCache(Tilemap* tm, TilesetCache& cache);
    // A gid's (index bits only) tileset and rect in the atlas's stored pixels,
    // through the LUT when there is one. Returns false for gid 0, gids in no
    // tileset, and tilesets whose header or atlas has not loaded.
    // The tile an animated tile shows at nowMs, as Tiled plays it; others as they are.
    static uint32_t Animate(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, uint32_t nowMs);

    static bool ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, int32_t& outTsIdx,
                            int32_t& outSx, int32_t& outSy, int32_t& outSw, int32_t& outSh, int32_t& outSpans,
                            int32_t& outOpaque);

    // The view copy. A group of a map's layers (what one DrawLayers call
    // draws), as DrawLayers drew it into a buffer of its own a margin larger
    // than the view, kept from frame to frame: while the camera moves by
    // whole pixels and the map stays as it is, a frame (or a part of one)
    // copies its pixels from there instead of drawing every layer's tiles
    // again, and only what comes into view is drawn, a band at a time.
    //
    // Only where the copy gives the very pixels drawing does: frames of
    // RGB565 or ARGB8888, not on a GPU; the map's own pixels at a whole
    // number of screen pixels each (1:1, or 2x, 3x... under the camera's
    // Pixel Perfect), on whole pixels, no parallax; no tint, opacity or blend
    // mode; tilesets whose pixels are each fully see-through or fully opaque;
    // no animated tile. On ARGB8888, only where the map is the first thing
    // drawn: there is no see-through copy in that format. Anything else is
    // drawn as before.
    //
    // The copy wraps around in both directions (a pixel's place is its
    // position modulo the copy's size), so the camera moving shifts nothing.
    struct ViewCopy
    {
        const TilemapComponent* tc = nullptr;
        uint32_t layerMask = 0;
        float shiftX = 0.0f, shiftY = 0.0f;
        uint64_t signature = 0;  // everything that decides its pixels but the camera
        // The camera it is drawn through, for the whole view: a pixel of the
        // copy at (x, y) is the one that camera puts at (x, y) on screen,
        // whatever the camera now. `valid` is in those coordinates.
        DekiRendering::FrameCamera anchor;
        int32_t width = 0, height = 0;
        // Drawn over the frame's clear colour (`clear`, as the frame stores
        // it): where the map comes first in a frame or a part of one, which
        // then copies its rows whole and needs no clear; in the frame's own
        // format (`format`, RGB565 or ARGB8888). Otherwise on its own,
        // see-through where the map draws nothing (RGB565A8), drawn over
        // whatever is there.
        bool opaque = false;
        Deki::ColorFormat format{};  // set when the copy is made
        uint8_t clear[4] = {};
        Deki::Buffer<uint8_t> pixels;
        int32_t BytesPerPixel() const;
        Deki::Rect valid{};
        // The band being drawn next to `valid`, a piece a frame (where the
        // copy wraps), and how many of its pieces are drawn: `valid` takes it
        // in once all are.
        Deki::Rect band{};
        int bandPieces = 0;
        uint32_t preparedFrame = 0;        // RenderContext::contentFrame it was brought up to date for
        bool usable = false;               // this frame: covers the view, at a whole-pixel offset
        int32_t offsetX = 0, offsetY = 0;  // this frame: screen (buffer) pixel + offset = copy coordinates
        bool off = false;  // drew an animated tile, or there is no memory: drawn as before until it changes
        uint32_t lastUse = 0;
    };
    std::vector<ViewCopy> m_ViewCopies;
    uint32_t m_CopyClock = 0;
    bool m_SaidCopying = false;      // the log says once that a map is drawn from a copy
    QuadBlit::ClipState m_ClipSwap;  // the frame's clips, while a copy is drawn
    // While a copy is drawn: the world point at its centre, which DrawLayers
    // picks chunks around instead of the camera's.
    bool m_CopyCentre = false;
    float m_CopyCentreX = 0.0f, m_CopyCentreY = 0.0f;
    // Chunks a DrawLayers call needed that were not loaded yet.
    uint32_t m_MissedChunks = 0;
    void NoteMissing(TilemapStreamer* streamer, uint32_t layer, int32_t chunkX, int32_t chunkY)
    {
        if (streamer->Has(static_cast<int32_t>(layer), chunkX, chunkY))
        {
            ++m_MissedChunks;
        }
    }
    // What Execute drew for an object in a frame redrawn in parts
    // (RenderContext::contentFrame): when every group of its layers came
    // from its copy, the frame's other parts are copied from them without
    // working any of it out again.
    struct FramePlan
    {
        // A group of layers drawn together, and the box on screen its tiles
        // can reach (`boxed`; a map that loops reaches everywhere).
        struct Group
        {
            uint32_t layerMask = 0;
            bool copied = false;
            bool boxed = false;
            float boxLeft = 0.0f, boxTop = 0.0f, boxRight = 0.0f, boxBottom = 0.0f;
        };
        const Deki::Object* obj = nullptr;
        const TilemapComponent* tc = nullptr;
        uint32_t frame = 0;  // the frame it holds for; 0 for none
        const uint8_t* buffer = nullptr;
        int32_t width = 0, height = 0, viewWidth = 0, viewHeight = 0;
        bool fromCopies = false;  // every group came from its copy
        std::vector<Group> groups;
    };
    std::vector<FramePlan> m_FramePlans;
    FramePlan* FindFramePlan(const Deki::Object* obj);
    // The object's plan, emptied for this frame.
    FramePlan& StartFramePlan(const Deki::Object* obj, const TilemapComponent* tc,
                              const DekiRendering::RenderContext& ctx);
    // Draws the part of the frame ctx describes from the copies the frame's
    // plan names; false, with nothing drawn, when it cannot.
    bool DrawPartFromCopies(Deki::Object* obj, const TilemapComponent* tc, DekiRendering::RenderContext& ctx);
    // Copies `target` (buffer pixels) from a copy brought up to date for the
    // frame; false, with nothing drawn, when the copy does not hold all of it.
    bool CopyPart(ViewCopy& copy, DekiRendering::RenderContext& ctx, const Deki::Rect& target);
    // Draws a group of layers from its copy, bringing the copy up to date
    // first; false when the group must be drawn as before. `group` is told
    // what it found, for the frame's plan.
    bool DrawFromCopy(Deki::Object* obj, DekiRendering::RenderContext& ctx, uint32_t layerMask, float shiftX,
                      float shiftY, uint32_t layerTint, float layerOpacity, uint8_t blendMode,
                      FramePlan::Group* group = nullptr);
    // Draws `rect` (copy coordinates) of a group into its copy, through
    // DrawLayers; false when it could not be drawn whole.
    bool DrawIntoCopy(ViewCopy& copy, Deki::Object* obj, const DekiRendering::RenderContext& ctx,
                      const Deki::Rect& rect, uint32_t layerMask, float shiftX, float shiftY, uint32_t layerTint,
                      float layerOpacity);
};

}  // namespace DekiTiledMap
