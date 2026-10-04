#include "TilemapRenderSystem.h"
#include "PixelFormat.h"
#include <deki/assets/Texture2D.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <deki/Object.h>
#include <deki/LogSystem.h>
#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"
#include "deki-rendering/DekiRenderPassRegistry.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-2d/Sprite.h"
#include "Tilemap.h"
#include "TilemapComponent.h"
#include "TilemapStreamer.h"
#include "Tileset.h"
#include <deki/assets/AssetManager.h>

namespace DekiTiledMap
{

namespace
{

// Chunk read budget per frame: 8 KiB, about 8 chunks at the default 16x16
// chunk size. Kept low for ESP32 SD reads.
constexpr size_t kIOByteBudgetPerFrame = 8 * 1024;

// A Source that points at the tileset atlas's pixels, without copying. Each
// tile is drawn by pointing the Source at the tile's slice with stride = atlas
// row bytes, so QuadBlit walks rows by stride and neighbouring tiles never
// bleed in. The tileset's chroma key (Tiled "transparentcolor") becomes a
// per-pixel skip in QuadBlit; for RGB565 atlases the key is reduced to 5/6/5
// precision so it matches what QuadBlit reads. Returns false if the atlas is
// not loaded yet.
bool MakeAtlasSource(Tileset* ts, QuadBlit::Source& outSrc)
{
    if (!ts)
    {
        return false;
    }
    Deki2D::Sprite* atlas = ts->Atlas();
    if (!atlas || !atlas->data)
    {
        return false;
    }

    const uint32_t bpp = Deki::Texture2D::GetBytesPerPixel(atlas->format);
    outSrc.pixels = atlas->data;
    outSrc.width = atlas->width;
    outSrc.height = atlas->height;
    outSrc.bytesPerPixel = static_cast<int32_t>(bpp);
    outSrc.hasAlpha = atlas->hasAlpha;
    outSrc.alphaOffset = atlas->hasAlpha ? static_cast<uint8_t>(bpp - 1) : 0;
    outSrc.isRGB565 = (atlas->format == Deki::Texture2D::TextureFormat::RGB565 ||
                       atlas->format == Deki::Texture2D::TextureFormat::RGB565A8);
    outSrc.alphaRowSpans = nullptr;
    outSrc.ownsPixels = false;
    outSrc.stride = atlas->width * static_cast<int32_t>(bpp);

    if (ts->HasTransparentColor())
    {
        uint8_t kr = ts->TransparentR();
        uint8_t kg = ts->TransparentG();
        uint8_t kb = ts->TransparentB();
        // RGB565 and RGB565A8 atlases lose the low bits on import, so the key
        // is reduced to the same precision or it would never match.
        if (outSrc.isRGB565)
        {
            DekiPixel::QuantizeRGB565(kr, kg, kb);
        }
        outSrc.hasChromaKey = true;
        outSrc.keyR = kr;
        outSrc.keyG = kg;
        outSrc.keyB = kb;
    }
    else
    {
        outSrc.hasChromaKey = false;
        outSrc.keyR = outSrc.keyG = outSrc.keyB = 0;
    }
    return true;
}

}  // namespace

TilemapRenderPass::TilesetCache& TilemapRenderPass::GetCache(Tilemap* tm)
{
    for (auto& entry : m_MCaches)
    {
        if (entry.first == tm)
        {
            return entry.second;
        }
    }
    m_MCaches.emplace_back(tm, TilesetCache{});
    auto& cache = m_MCaches.back().second;
    const auto& refs = tm->Tilesets();
    cache.tilesets.assign(refs.size(), nullptr);
    cache.sources.assign(refs.size(), QuadBlit::Source{});
    cache.ready.assign(refs.size(), false);
    cache.destW.assign(refs.size(), 0);
    cache.destH.assign(refs.size(), 0);
    cache.scratch.assign(refs.size(), QuadBlit::Source{});
    // Start at the current epoch so the first RefreshCache does not wipe the
    // new vectors. A later UnloadAll or InvalidateAsset moves the epoch and is
    // noticed.
    if (auto* mgr = Deki::AssetManager::Get())
    {
        cache.epoch = mgr->GetEpoch();
    }
    return cache;
}

void TilemapRenderPass::RefreshCache(Tilemap* tm, TilesetCache& cache)
{
    auto* mgr = Deki::AssetManager::Get();
    if (!mgr)
    {
        return;
    }

    // The cached Source.pixels point into atlas memory the AssetManager owns.
    // UnloadAll, InvalidateAsset and hot reload free that memory and move the
    // global epoch. When it moved, drop every cached Tileset* and Source so the
    // loop below resolves them again.
    const uint64_t curEpoch = mgr->GetEpoch();
    if (cache.epoch != curEpoch)
    {
        std::fill(cache.tilesets.begin(), cache.tilesets.end(), nullptr);
        std::fill(cache.ready.begin(), cache.ready.end(), false);
        for (auto& src : cache.sources)
        {
            src = QuadBlit::Source{};
        }
        cache.gidLut.clear();
        cache.gidLimit = 0;
        cache.epoch = curEpoch;
    }

    const auto& refs = tm->Tilesets();
    for (size_t i = 0; i < refs.size(); ++i)
    {
        // Resolve again any tileset whose atlas was not ready. Once ready, the
        // cached Source stays valid until the epoch moves.
        Tileset* ts = cache.tilesets[i];
        if (!ts)
        {
            ts = static_cast<Tileset*>(mgr->LoadByGuidAndType(refs[i].guid, Tileset::kAssetTypeName));
            cache.tilesets[i] = ts;
        }
        if (!cache.ready[i])
        {
            cache.ready[i] = MakeAtlasSource(ts, cache.sources[i]);
        }
    }

    // The gid range is known once every tileset header is in (atlases may
    // still be loading). Tile counts come from the baked headers.
    if (cache.gidLimit == 0 && !refs.empty())
    {
        uint32_t limit = 0;
        bool allLoaded = true;
        for (size_t i = 0; i < refs.size(); ++i)
        {
            if (!cache.tilesets[i])
            {
                allLoaded = false;
                break;
            }
            limit = std::max(limit, refs[i].firstGid + cache.tilesets[i]->TileCount());
        }
        if (allLoaded && limit > 0)
        {
            cache.gidLimit = limit;
            cache.gidLut.assign(limit, TileLUT{ kUnresolved, 0, 0, 0, 0 });
        }
    }
}

namespace
{
// A tileset rect (in the atlas image's pixels) in the atlas's stored pixels.
// The same numbers unless Max Size shrank the atlas; edges are mapped, so
// tiles that touch in the image still touch.
void ToStoredRect(const Deki2D::Sprite* atlas, int sx, int sy, int sw, int sh, int32_t& x, int32_t& y, int32_t& w,
                  int32_t& h)
{
    x = atlas->SourceToStoredX(sx);
    y = atlas->SourceToStoredY(sy);
    w = std::max<int32_t>(1, atlas->SourceToStoredX(sx + sw) - x);
    h = std::max<int32_t>(1, atlas->SourceToStoredY(sy + sh) - y);
}
}  // namespace

bool TilemapRenderPass::ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, int32_t& outTsIdx,
                                    int32_t& outSx, int32_t& outSy, int32_t& outSw, int32_t& outSh)
{
    if (gidIndex == 0)
    {
        return false;
    }

    if (cache.gidLimit != 0)
    {
        if (gidIndex >= cache.gidLimit)
        {
            return false;  // beyond every tileset
        }
        TileLUT& e = cache.gidLut[gidIndex];
        if (e.tsIdx == kUnresolved)
        {
            uint32_t localId = 0;
            size_t tsIdx = 0;
            const TilesetRef* tref = tm->ResolveTilesetWithIndex(gidIndex, localId, tsIdx);
            Tileset* ts = tref ? cache.tilesets[tsIdx] : nullptr;
            if (!ts || localId >= ts->TileCount())
            {
                e.tsIdx = kUnmapped;
            }
            else if (!ts->Atlas())
            {
                return false;  // stored size not known yet; resolve once it loads
            }
            else
            {
                int sx, sy, sw, sh;
                ts->GetTileRect(localId, sx, sy, sw, sh);
                TileLUT entry{ static_cast<int32_t>(tsIdx), 0, 0, 0, 0 };
                ToStoredRect(ts->Atlas(), sx, sy, sw, sh, entry.sx, entry.sy, entry.sw, entry.sh);
                e = entry;
            }
        }
        if (e.tsIdx < 0)
        {
            return false;
        }
        outTsIdx = e.tsIdx;
        outSx = e.sx;
        outSy = e.sy;
        outSw = e.sw;
        outSh = e.sh;
        return true;
    }

    // Some tileset header is still loading: resolve without caching.
    uint32_t localId = 0;
    size_t tsIdx = 0;
    const TilesetRef* tref = tm->ResolveTilesetWithIndex(gidIndex, localId, tsIdx);
    if (!tref || tsIdx >= cache.tilesets.size() || !cache.tilesets[tsIdx])
    {
        return false;
    }
    Tileset* ts = cache.tilesets[tsIdx];
    if (!ts->Atlas())
    {
        return false;
    }
    int sx, sy, sw, sh;
    ts->GetTileRect(localId, sx, sy, sw, sh);
    outTsIdx = static_cast<int32_t>(tsIdx);
    ToStoredRect(ts->Atlas(), sx, sy, sw, sh, outSx, outSy, outSw, outSh);
    return true;
}

void TilemapRenderPass::Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    if (!obj)
    {
        return;
    }
    auto* tc = obj->GetComponent<TilemapComponent>();
    if (!tc)
    {
        return;
    }
    Tilemap* tm = tc->tilemap.Get();
    if (!tm)
    {
        return;
    }
    if (!ctx.camera || !ctx.buffer || !ctx.cam.valid)
    {
        return;
    }

    const int32_t screenW = ctx.width;
    const int32_t screenH = ctx.height;

    const int tw = tm->TileWidth();
    const int th = tm->TileHeight();
    const int cw = tm->ChunkWidth();
    const int ch = tm->ChunkHeight();
    if (tw <= 0 || th <= 0 || cw <= 0 || ch <= 0)
    {
        return;
    }

    // The map's pixels per world meter. Map-pixel values below are divided by
    // tilePPM to get meters, to match the owner's transform (meters) and the
    // camera (pixels per meter).
    const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
    const float invTilePPM = 1.0f / tilePPM;

    // Which map point sits on the owner's world position:
    //   Finite map:           the map's centre.
    //   Infinite with origin: a Tiled object named "origin", placed wherever
    //                         world (0, 0) should be.
    //   Infinite, no origin:  Tiled (0, 0).
    // Y is flipped: Tiled rows run top to bottom (Y down) and the engine is
    // Y up, so Tiled row 0 lands at engine Y = +originOffsetY.
    const float originX = (obj->GetWorldX());
    const float originY = (obj->GetWorldY());
    float originOffsetX = 0.0f;
    float originOffsetY = 0.0f;
    if (!tm->IsInfinite())
    {
        // Half the map's size, in meters.
        originOffsetX = 0.5f * static_cast<float>(tm->MapWidth()) * static_cast<float>(tw) * invTilePPM;
        originOffsetY = 0.5f * static_cast<float>(tm->MapHeight()) * static_cast<float>(th) * invTilePPM;
    }
    else
    {
        // FindOrigin returns Tiled pixels; convert to meters.
        tm->FindOrigin(originOffsetX, originOffsetY);
        originOffsetX *= invTilePPM;
        originOffsetY *= invTilePPM;
    }

    // The camera's visible rect in map pixels (Y down), to pick chunks. Sizes
    // are meters, converted with tilePPM. Visible size = screen / ppm
    // (DekiRendering::CameraComponent::GetVisibleWidth). The position comes
    // from the camera, not the snapshot, which may be pixel-snapped.
    const float visW = (ctx.cam.ppm > 0.0f) ? (static_cast<float>(screenW) / ctx.cam.ppm) : 0.0f;
    const float visH = (ctx.cam.ppm > 0.0f) ? (static_cast<float>(screenH) / ctx.cam.ppm) : 0.0f;
    const float camX = ctx.camera->GetPositionX();
    const float camY = ctx.camera->GetPositionY();

    // Map pixels (meters * tilePPM) for the chunk math.
    const float tiledMinX = ((camX - originX) + originOffsetX - visW * 0.5f) * tilePPM;
    const float tiledMaxX = ((camX - originX) + originOffsetX + visW * 0.5f) * tilePPM;
    const float tiledMinY = (originOffsetY - ((camY - originY) + visH * 0.5f)) * tilePPM;
    const float tiledMaxY = (originOffsetY - ((camY - originY) - visH * 0.5f)) * tilePPM;

    auto floorDiv = [](int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); };

    const int chunkMinX = floorDiv(static_cast<int>(std::floor(tiledMinX)) / tw, cw) - tc->chunkPadding;
    const int chunkMinY = floorDiv(static_cast<int>(std::floor(tiledMinY)) / th, ch) - tc->chunkPadding;
    const int chunkMaxX = floorDiv(static_cast<int>(std::floor(tiledMaxX)) / tw, cw) + tc->chunkPadding;
    const int chunkMaxY = floorDiv(static_cast<int>(std::floor(tiledMaxY)) / th, ch) + tc->chunkPadding;

    auto* streamer = tm->Streamer();
    if (!streamer)
    {
        return;
    }

    // Wrap periods: wrapPeriodX/Y when above 0, else the map's bounds, for
    // each axis that loops. A period is in tiles and rounded down to whole
    // chunks, so make repeats a multiple of the chunk size.
    int periodTilesX = 0;
    int periodTilesY = 0;
    int originTileX = 0;
    int originTileY = 0;
    if (tc->loopX || tc->loopY)
    {
        int32_t bx = 0, by = 0, bw = 0, bh = 0;
        const bool haveBounds = tm->GetAuthoredBounds(bx, by, bw, bh);
        if (tc->loopX)
        {
            if (tc->wrapPeriodX > 0)
            {
                periodTilesX = tc->wrapPeriodX;
            }
            else if (haveBounds)
            {
                periodTilesX = bw;
                originTileX = bx;
            }
        }
        if (tc->loopY)
        {
            if (tc->wrapPeriodY > 0)
            {
                periodTilesY = tc->wrapPeriodY;
            }
            else if (haveBounds)
            {
                periodTilesY = bh;
                originTileY = by;
            }
        }
    }
    const int periodChunksX = (periodTilesX > 0) ? (periodTilesX / cw) : 0;
    const int periodChunksY = (periodTilesY > 0) ? (periodTilesY / ch) : 0;
    const int originChunksX = (cw > 0) ? originTileX / cw : 0;
    const int originChunksY = (ch > 0) ? originTileY / ch : 0;
    const bool wrapX = periodChunksX > 0;
    const bool wrapY = periodChunksY > 0;

    auto wrap = [](int v, int n)
    {
        int r = v % n;
        return r < 0 ? r + n : r;
    };

    // Request only chunks that exist in the map: those in the period when
    // wrapping, else the visible rect. Repeats reuse the same loaded chunk.
    int reqMinX = chunkMinX, reqMaxX = chunkMaxX;
    int reqMinY = chunkMinY, reqMaxY = chunkMaxY;
    if (wrapX)
    {
        const int span = chunkMaxX - chunkMinX;
        if (span >= periodChunksX - 1)
        {
            reqMinX = originChunksX;
            reqMaxX = originChunksX + periodChunksX - 1;
        }
        else
        {
            reqMinX = originChunksX + wrap(chunkMinX - originChunksX, periodChunksX);
            reqMaxX = reqMinX + span;
        }
    }
    if (wrapY)
    {
        const int span = chunkMaxY - chunkMinY;
        if (span >= periodChunksY - 1)
        {
            reqMinY = originChunksY;
            reqMaxY = originChunksY + periodChunksY - 1;
        }
        else
        {
            reqMinY = originChunksY + wrap(chunkMinY - originChunksY, periodChunksY);
            reqMaxY = reqMinY + span;
        }
    }

    // Request and load chunks for every visible layer.
    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visibleLayerMask >> layer) & 1) == 0)
        {
            continue;
        }
        if (wrapX || wrapY)
        {
            // The request may cross the period boundary, so split it into up
            // to two ranges per axis, each inside [0, period).
            const int periodEndX = originChunksX + periodChunksX;
            const int periodEndY = originChunksY + periodChunksY;
            const int xs[2] = { reqMinX, originChunksX };
            const int xe[2] = { wrapX && reqMaxX >= periodEndX ? periodEndX - 1 : reqMaxX,
                                wrapX && reqMaxX >= periodEndX ? reqMaxX - periodChunksX : -1 };
            const int ys[2] = { reqMinY, originChunksY };
            const int ye[2] = { wrapY && reqMaxY >= periodEndY ? periodEndY - 1 : reqMaxY,
                                wrapY && reqMaxY >= periodEndY ? reqMaxY - periodChunksY : -1 };
            for (int iy = 0; iy < 2; ++iy)
            {
                if (ye[iy] < ys[iy])
                {
                    continue;
                }
                for (int ix = 0; ix < 2; ++ix)
                {
                    if (xe[ix] < xs[ix])
                    {
                        continue;
                    }
                    streamer->RequestRect(static_cast<int32_t>(layer), xs[ix], ys[iy], xe[ix], ye[iy]);
                }
            }
        }
        else
        {
            streamer->RequestRect(static_cast<int32_t>(layer), chunkMinX, chunkMinY, chunkMaxX, chunkMaxY);
        }
    }
    streamer->Pump(kIOByteBudgetPerFrame);

    // Resolved tilesets and their Sources, built once per Tilemap and kept
    // across frames; entries whose atlas has not loaded are tried each frame.
    // The whole table is dropped when the asset epoch moves, since its
    // Tilemap* keys are asset pointers.
    if (auto* mgr = Deki::AssetManager::Get())
    {
        const uint64_t epoch = mgr->GetEpoch();
        if (epoch != m_CachesEpoch)
        {
            m_MCaches.clear();
            m_CachesEpoch = epoch;
        }
    }
    TilesetCache& cache = GetCache(tm);
    RefreshCache(tm, cache);

    // Map each visible chunk coordinate to the map's chunk once per axis
    // (unchanged without wrap), instead of a modulo per cell in the loops.
    if (wrapX || wrapY)
    {
        const int spanX = chunkMaxX - chunkMinX + 1;
        const int spanY = chunkMaxY - chunkMinY + 1;
        m_SrcChunkXLut.resize(static_cast<size_t>(spanX));
        m_SrcChunkYLut.resize(static_cast<size_t>(spanY));
        for (int i = 0; i < spanX; ++i)
        {
            const int cx = chunkMinX + i;
            m_SrcChunkXLut[i] = wrapX ? originChunksX + wrap(cx - originChunksX, periodChunksX) : cx;
        }
        for (int i = 0; i < spanY; ++i)
        {
            const int cy = chunkMinY + i;
            m_SrcChunkYLut[i] = wrapY ? originChunksY + wrap(cy - originChunksY, periodChunksY) : cy;
        }
    }

    const uint8_t tintR = tc->tintColor.r;
    const uint8_t tintG = tc->tintColor.g;
    const uint8_t tintB = tc->tintColor.b;
    const uint8_t tintA = tc->tintColor.a;
    const bool pixelSnap = tc->pixelSnap;

    // Everything below maps through the frame's camera snapshot: the same math
    // as DekiRendering::CameraComponent::WorldToScreen, without a virtual call
    // per tile.
    const DekiRendering::FrameCamera& cam = ctx.cam;

    // Map pixels / tilePPM = meters; * camera PPM = screen pixels. The scale is
    // camera PPM / tilePPM, 1:1 when they match. Every tile of a tileset has
    // the same drawn size, so it is worked out once per tileset per frame, and
    // the tileset's scratch Source is set up here; tiles change only its pixel
    // pointer and flip flags.
    const float scale = cam.ppm * invTilePPM;
    int32_t maxDestW = 0, maxDestH = 0;
    for (size_t i = 0; i < cache.sources.size(); ++i)
    {
        if (!cache.ready[i])
        {
            continue;
        }
        const Tileset* ts = cache.tilesets[i];
        cache.destW[i] = static_cast<int32_t>(std::floor(static_cast<float>(ts->TileWidth()) * scale));
        cache.destH[i] = static_cast<int32_t>(std::floor(static_cast<float>(ts->TileHeight()) * scale));
        cache.scratch[i] = cache.sources[i];
        // Width and height are set per tile (a shrunk atlas's tiles can differ
        // by a pixel); scratch.stride stays the atlas row width.
        maxDestW = std::max(maxDestW, cache.destW[i]);
        maxDestH = std::max(maxDestH, cache.destH[i]);
    }

    // The rect BlitScaled can write: the target within the current clip. A
    // tile or chunk outside it is skipped before any Source work; BlitScaled
    // would clip it away anyway.
    const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
    const int32_t clipL = std::max<int32_t>(0, clip.left);
    const int32_t clipT = std::max<int32_t>(0, clip.top);
    const int32_t clipR = std::min<int32_t>(screenW, clip.right);
    const int32_t clipB = std::min<int32_t>(screenH, clip.bottom);
    if (clipL >= clipR || clipT >= clipB)
    {
        return;
    }

    ++m_FrameSerial;
    if (m_FrameSerial == 0)
    {
        ++m_FrameSerial;  // 0 means "never touched" in the streamer
    }

    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visibleLayerMask >> layer) & 1) == 0)
        {
            continue;
        }

        m_DrawsScratch.clear();

        if (wrapX || wrapY)
        {
            const int spanX = chunkMaxX - chunkMinX + 1;
            const int spanY = chunkMaxY - chunkMinY + 1;
            m_DrawsScratch.reserve(static_cast<size_t>(spanX) * static_cast<size_t>(spanY));
            for (int iy = 0; iy < spanY; ++iy)
            {
                for (int ix = 0; ix < spanX; ++ix)
                {
                    m_DrawsScratch.push_back(
                        { chunkMinX + ix, chunkMinY + iy, m_SrcChunkXLut[ix], m_SrcChunkYLut[iy] });
                }
            }
        }
        else
        {
            tm->QueryVisibleChunks(static_cast<int32_t>(layer), chunkMinX, chunkMinY, chunkMaxX, chunkMaxY,
                                   m_VisibleScratch);
            m_DrawsScratch.reserve(m_VisibleScratch.size());
            for (const auto& entry : m_VisibleScratch)
            {
                m_DrawsScratch.push_back({ entry.chunkX, entry.chunkY, entry.chunkX, entry.chunkY });
            }
        }

        for (const auto& d : m_DrawsScratch)
        {
            const int chunkOriginX = d.drawX * cw * tw;
            const int chunkOriginY = d.drawY * ch * th;

            // The chunk's screen box, with margin (largest tile, 2 px for snap
            // rounding): skip padding chunks that cannot touch the clip.
            {
                float fx0, fy0;
                cam.WorldToScreen(originX + static_cast<float>(chunkOriginX) * invTilePPM - originOffsetX,
                                  originY + originOffsetY - static_cast<float>(chunkOriginY) * invTilePPM, fx0, fy0);
                const float spanX = static_cast<float>((cw - 1) * tw) * scale + static_cast<float>(maxDestW);
                const float spanY = static_cast<float>((ch - 1) * th) * scale + static_cast<float>(maxDestH);
                if (fx0 + spanX + 2.0f < static_cast<float>(clipL) || fx0 - 2.0f > static_cast<float>(clipR) ||
                    fy0 + spanY + 2.0f < static_cast<float>(clipT) || fy0 - 2.0f > static_cast<float>(clipB))
                {
                    continue;
                }
            }

            const TileChunk* chunk = streamer->GetAndTouch(static_cast<int32_t>(layer), d.srcX, d.srcY, m_FrameSerial);
            if (!chunk)
            {
                continue;
            }

            for (int ty = 0; ty < ch; ++ty)
            {
                for (int tx = 0; tx < cw; ++tx)
                {
                    const uint32_t gid = chunk->tileGids[ty * cw + tx];
                    int32_t tsIdx, sx, sy, sw, sh;
                    if (!ResolveTile(tm, cache, GidIndex(gid), tsIdx, sx, sy, sw, sh))
                    {
                        continue;
                    }
                    if (!cache.ready[tsIdx])
                    {
                        continue;
                    }

                    // The tile's top-left in Tiled pixels, converted to world
                    // meters (Y up, centred on the owner). WorldToScreen gets
                    // the tile's top-left in engine terms, the corner with the
                    // highest Y, which BlitScaled takes as (destX, destY).
                    const float tiledTileX = static_cast<float>(chunkOriginX + tx * tw) * invTilePPM;
                    const float tiledTileY = static_cast<float>(chunkOriginY + ty * th) * invTilePPM;
                    const float wx = originX + tiledTileX - originOffsetX;
                    const float wy = originY + originOffsetY - tiledTileY;

                    float fDestSX, fDestSY;
                    cam.WorldToScreen(wx, wy, fDestSX, fDestSY);
                    if (cam.snapStep > 0)  // project Pixel Perfect: the art-pixel grid
                    {
                        fDestSX = cam.SnapX(fDestSX);
                        fDestSY = cam.SnapY(fDestSY);
                    }
                    const int destSX = (pixelSnap || cam.snapStep > 0) ? static_cast<int>(std::lround(fDestSX))
                                                                       : static_cast<int>(fDestSX);
                    const int destSY = (pixelSnap || cam.snapStep > 0) ? static_cast<int>(std::lround(fDestSY))
                                                                       : static_cast<int>(fDestSY);

                    const int destW = cache.destW[tsIdx];
                    const int destH = cache.destH[tsIdx];
                    if (destW <= 0 || destH <= 0 || destSX >= clipR || destSX + destW <= clipL || destSY >= clipB ||
                        destSY + destH <= clipT)
                    {
                        continue;  // BlitScaled would clip this to nothing
                    }

                    // Point the scratch Source at this tile's slice of the
                    // atlas; the stride keeps QuadBlit on the atlas's full row
                    // width, so neighbouring tiles never bleed in. QuadBlit
                    // applies the tileset's chroma key per pixel. Tiled's flip
                    // flags go on the Source; BlitScaled rejects a negative size.
                    const QuadBlit::Source& base = cache.sources[tsIdx];
                    QuadBlit::Source& sub = cache.scratch[tsIdx];
                    sub.pixels = base.pixels + sy * base.stride + sx * base.bytesPerPixel;
                    sub.width = sw;  // stored pixels; the destination keeps the tileset's size
                    sub.height = sh;
                    sub.flipH = GidFlipH(gid);
                    sub.flipV = GidFlipV(gid);
                    sub.flipD = GidFlipD(gid);

                    QuadBlit::BlitScaled(sub, ctx.buffer, screenW, screenH, ctx.format, destSX, destSY, destW, destH,
                                         tintR, tintG, tintB, tintA);
                }
            }
        }
    }
}

}  // namespace DekiTiledMap

// Registers with autoAttach=true, so DekiRenderingInit attaches the pass to
// the active DekiRendering::Standard2DRenderer whenever this package is
// loaded. The project's .rpipeline need not mention "tilemap", but can, to
// order it against other passes (clip2d, say).
namespace
{
struct TilemapRenderPassRegistrar
{
    TilemapRenderPassRegistrar()
    {
        DekiRendering::RenderPassInfo info;
        info.factory = []() -> DekiRendering::RenderPass* { return new DekiTiledMap::TilemapRenderPass(); };
        info.autoAttach = true;
        DekiRendering::DekiRenderPassRegistry::Register(DekiTiledMap::TilemapRenderPass::kRegistryName, info);
    }
    // Unregister on DLL unload, so the factory (code in this package) does
    // not outlive the DLL and crash deki-rendering's registry teardown.
    ~TilemapRenderPassRegistrar()
    {
        DekiRendering::DekiRenderPassRegistry::Unregister(DekiTiledMap::TilemapRenderPass::kRegistryName);
    }
};
static TilemapRenderPassRegistrar s_TilemapPassRegistrar;
}  // namespace
