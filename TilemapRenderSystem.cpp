#include "TilemapRenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "DekiObject.h"
#include "DekiLogSystem.h"
#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"
#include "deki-rendering/DekiRenderPassRegistry.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-2d/Sprite.h"
#include "Tilemap.h"
#include "TilemapComponent.h"
#include "TilemapStreamer.h"
#include "Tileset.h"
#include "assets/AssetManager.h"

namespace DekiTilemap
{

namespace
{

// Per-frame IO budget for chunk reads. 8 KiB/frame ≈ 8 chunks at the default
// 16x16 chunk size. Conservative for ESP32 SD reads.
constexpr size_t kIOByteBudgetPerFrame = 8 * 1024;

// Build a Tileset Source descriptor referencing the atlas's pixel buffer
// directly (no copy). Each tile is rendered by pointing the Source at the
// tile's slice with stride = atlas row bytes — QuadBlit walks rows by stride
// so adjacent atlas tiles never bleed in. Tileset chroma-key (Tiled
// "transparentcolor") becomes a per-pixel skip inside QuadBlit; for RGB565
// atlases the key is pre-quantized to 5/6/5 precision so the compare matches
// the value QuadBlit extracts from source bytes. Returns false if the atlas
// isn't loaded yet.
bool MakeAtlasSource(Tileset* ts, QuadBlit::Source& outSrc)
{
    if (!ts) return false;
    Sprite* atlas = ts->Atlas();
    if (!atlas || !atlas->data) return false;

    const uint32_t bpp   = Texture2D::GetBytesPerPixel(atlas->format);
    outSrc.pixels        = atlas->data;
    outSrc.width         = atlas->width;
    outSrc.height        = atlas->height;
    outSrc.bytesPerPixel = static_cast<int32_t>(bpp);
    outSrc.hasAlpha      = atlas->hasAlpha;
    outSrc.alphaOffset   = atlas->hasAlpha ? static_cast<uint8_t>(bpp - 1) : 0;
    outSrc.isRGB565      = (atlas->format == Texture2D::TextureFormat::RGB565 ||
                            atlas->format == Texture2D::TextureFormat::RGB565A8);
    outSrc.alphaRowSpans = nullptr;
    outSrc.ownsPixels    = false;
    outSrc.stride        = atlas->width * static_cast<int32_t>(bpp);

    if (ts->HasTransparentColor())
    {
        uint8_t kr = ts->TransparentR();
        uint8_t kg = ts->TransparentG();
        uint8_t kb = ts->TransparentB();
        // Quantize to RGB565 precision for RGB565/RGB565A8 atlases: PNGs
        // imported into 5/6/5 lose low bits, so an exact 8-bit compare
        // against the authored key would never match.
        if (outSrc.isRGB565)
        {
            kr = static_cast<uint8_t>((kr >> 3) << 3);
            kg = static_cast<uint8_t>((kg >> 2) << 2);
            kb = static_cast<uint8_t>((kb >> 3) << 3);
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

} // namespace

TilemapRenderPass::TilesetCache& TilemapRenderPass::GetCache(Tilemap* tm)
{
    for (auto& entry : m_MCaches)
        if (entry.first == tm) return entry.second;
    m_MCaches.emplace_back(tm, TilesetCache{});
    auto& cache = m_MCaches.back().second;
    const auto& refs = tm->Tilesets();
    cache.tilesets.assign(refs.size(), nullptr);
    cache.sources.assign(refs.size(), QuadBlit::Source{});
    cache.ready.assign(refs.size(), false);
    // Seed the epoch so the very first RefreshCache doesn't immediately wipe
    // the freshly-initialised vectors. A bump from any later UnloadAll /
    // InvalidateAsset will be picked up because it advances the epoch.
    if (auto* mgr = Deki::AssetManager::Get())
        cache.epoch = mgr->GetEpoch();
    return cache;
}

void TilemapRenderPass::RefreshCache(Tilemap* tm, TilesetCache& cache)
{
    auto* mgr = Deki::AssetManager::Get();
    if (!mgr) return;

    // The cached Source.pixels are raw pointers into atlas memory owned by
    // AssetManager. UnloadAll / InvalidateAsset / hot-reload free that memory
    // and bump the global epoch. If our epoch is stale, drop every cached
    // Tileset* + Source so the loop below re-resolves through AssetManager.
    const uint64_t curEpoch = mgr->GetEpoch();
    if (cache.epoch != curEpoch)
    {
        std::fill(cache.tilesets.begin(), cache.tilesets.end(), nullptr);
        std::fill(cache.ready.begin(), cache.ready.end(), false);
        for (auto& src : cache.sources)
            src = QuadBlit::Source{};
        cache.epoch = curEpoch;
    }

    const auto& refs = tm->Tilesets();
    for (size_t i = 0; i < refs.size(); ++i)
    {
        // Re-resolve any tileset whose atlas hasn't been ready yet. Once
        // ready, the cached Source stays valid until the epoch bump above
        // invalidates it.
        Tileset* ts = cache.tilesets[i];
        if (!ts)
        {
            ts = static_cast<Tileset*>(
                mgr->LoadByGuidAndType(refs[i].guid, Tileset::AssetTypeName));
            cache.tilesets[i] = ts;
        }
        if (!cache.ready[i])
            cache.ready[i] = MakeAtlasSource(ts, cache.sources[i]);
    }
}

void TilemapRenderPass::Execute(DekiObject* obj, RenderContext& ctx)
{
    if (!obj) return;
    auto* tc = obj->GetComponent<TilemapComponent>();
    if (!tc) return;
    Tilemap* tm = tc->tilemap.Get();
    if (!tm) return;
    if (!ctx.camera || !ctx.buffer) return;

    const int32_t screenW = ctx.width;
    const int32_t screenH = ctx.height;

    const int tw = tm->TileWidth();
    const int th = tm->TileHeight();
    const int cw = tm->ChunkWidth();
    const int ch = tm->ChunkHeight();
    if (tw <= 0 || th <= 0 || cw <= 0 || ch <= 0) return;

    // Tilemap's source pixels per world meter. All tile-pixel quantities
    // below are converted to meters by dividing by tilePPM, so the math
    // composes cleanly with the owner transform (already meters) and camera
    // (already pixels-per-meter).
    const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
    const float invTilePPM = 1.0f / tilePPM;

    // What world-meter coordinate maps to the GameObject's world position?
    //   Finite map:        the map's center — keeps the whole rect on the owner.
    //   Infinite + origin: a Tiled object named "origin" — author places it
    //                      wherever they want world (0, 0) to be.
    //   Infinite, no origin: Tiled (0, 0), strict coord mapping (back-compat).
    // Y is flipped here because Tiled stores rows top-to-bottom (Y+ down) and
    // the engine is Y-up, so Tiled row 0 ends up at engine Y = +originOffsetY.
    const float originX = (obj->GetWorldX());
    const float originY = (obj->GetWorldY());
    float originOffsetX = 0.0f;
    float originOffsetY = 0.0f;
    if (!tm->IsInfinite())
    {
        // Source-pixel half-extents converted to meters.
        originOffsetX = 0.5f * static_cast<float>(tm->MapWidth())  * static_cast<float>(tw) * invTilePPM;
        originOffsetY = 0.5f * static_cast<float>(tm->MapHeight()) * static_cast<float>(th) * invTilePPM;
    }
    else
    {
        // FindOrigin returns Tiled pixels — convert to meters.
        tm->FindOrigin(originOffsetX, originOffsetY);
        originOffsetX *= invTilePPM;
        originOffsetY *= invTilePPM;
    }

    // Camera visible rect, expressed in tile-pixel coords (Y+ down) for chunk
    // selection. Camera/visible sizes are meters; convert via tilePPM.
    const float visW = ctx.camera->GetVisibleWidth(screenW);
    const float visH = ctx.camera->GetVisibleHeight(screenH);
    const float camX = ctx.camera->GetPositionX();
    const float camY = ctx.camera->GetPositionY();

    // Switch to tile-pixel space (meters * tilePPM) for chunk math.
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
    if (!streamer) return;

    // Resolve wrap periods. auto_wrap pulls them from authored bounds;
    // otherwise use the manual wrapPeriodX/y fields (0 disables an axis).
    // Period is interpreted as tiles and floor-divided to chunks — sub-chunk
    // remainders are silently dropped, so size strips on chunk boundaries.
    int periodTilesX = 0;
    int periodTilesY = 0;
    int originTileX  = 0;
    int originTileY  = 0;
    if (tc->loopX || tc->loopY)
    {
        int32_t bx = 0, by = 0, bw = 0, bh = 0;
        const bool haveBounds = tm->GetAuthoredBounds(bx, by, bw, bh);
        if (tc->loopX)
        {
            if (tc->wrapPeriodX > 0)   periodTilesX = tc->wrapPeriodX;
            else if (haveBounds)         { periodTilesX = bw; originTileX = bx; }
        }
        if (tc->loopY)
        {
            if (tc->wrapPeriodY > 0)   periodTilesY = tc->wrapPeriodY;
            else if (haveBounds)         { periodTilesY = bh; originTileY = by; }
        }
    }
    const int periodChunksX = (periodTilesX > 0) ? (periodTilesX / cw) : 0;
    const int periodChunksY = (periodTilesY > 0) ? (periodTilesY / ch) : 0;
    const int originChunksX = (cw > 0) ? originTileX / cw : 0;
    const int originChunksY = (ch > 0) ? originTileY / ch : 0;
    const bool wrapX = periodChunksX > 0;
    const bool wrapY = periodChunksY > 0;

    auto wrap = [](int v, int n) { int r = v % n; return r < 0 ? r + n : r; };

    // For streaming, only request authored chunks (those inside the period
    // window when wrapping; otherwise the unwrapped visible rect). Repeated
    // tiles reuse the same resident chunk.
    int reqMinX = chunkMinX, reqMaxX = chunkMaxX;
    int reqMinY = chunkMinY, reqMaxY = chunkMaxY;
    if (wrapX)
    {
        const int span = chunkMaxX - chunkMinX;
        if (span >= periodChunksX - 1) { reqMinX = originChunksX; reqMaxX = originChunksX + periodChunksX - 1; }
        else
        {
            reqMinX = originChunksX + wrap(chunkMinX - originChunksX, periodChunksX);
            reqMaxX = reqMinX + span;
        }
    }
    if (wrapY)
    {
        const int span = chunkMaxY - chunkMinY;
        if (span >= periodChunksY - 1) { reqMinY = originChunksY; reqMaxY = originChunksY + periodChunksY - 1; }
        else
        {
            reqMinY = originChunksY + wrap(chunkMinY - originChunksY, periodChunksY);
            reqMaxY = reqMinY + span;
        }
    }

    // Request + pump for every visible layer this frame.
    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visibleLayerMask >> layer) & 1) == 0) continue;
        if (wrapX || wrapY)
        {
            // Request may straddle the period boundary; split into up to two
            // ranges per axis so each piece lands inside [0, period).
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
                if (ye[iy] < ys[iy]) continue;
                for (int ix = 0; ix < 2; ++ix)
                {
                    if (xe[ix] < xs[ix]) continue;
                    streamer->RequestRect(static_cast<int32_t>(layer),
                                          xs[ix], ys[iy], xe[ix], ye[iy]);
                }
            }
        }
        else
        {
            streamer->RequestRect(static_cast<int32_t>(layer),
                                  chunkMinX, chunkMinY, chunkMaxX, chunkMaxY);
        }
    }
    streamer->Pump(kIOByteBudgetPerFrame);

    // Resolved tilesets + per-tileset Source descriptors. Built once per
    // Tilemap and reused across frames; entries with not-yet-loaded atlases
    // are retried each frame.
    TilesetCache& cache = GetCache(tm);
    RefreshCache(tm, cache);

    // Precompute the unwrapped→authored chunk-coord mapping per axis once.
    // Identity when no wrap on that axis. Avoids a modulo+lambda call per
    // visible cell inside the inner loops.
    if (wrapX || wrapY)
    {
        const int spanX = chunkMaxX - chunkMinX + 1;
        const int spanY = chunkMaxY - chunkMinY + 1;
        m_srcChunkXLut.resize(static_cast<size_t>(spanX));
        m_srcChunkYLut.resize(static_cast<size_t>(spanY));
        for (int i = 0; i < spanX; ++i)
        {
            const int cx = chunkMinX + i;
            m_srcChunkXLut[i] = wrapX ? originChunksX + wrap(cx - originChunksX, periodChunksX) : cx;
        }
        for (int i = 0; i < spanY; ++i)
        {
            const int cy = chunkMinY + i;
            m_srcChunkYLut[i] = wrapY ? originChunksY + wrap(cy - originChunksY, periodChunksY) : cy;
        }
    }

    const uint8_t tintR = tc->tintColor.r;
    const uint8_t tintG = tc->tintColor.g;
    const uint8_t tintB = tc->tintColor.b;
    const uint8_t tintA = tc->tintColor.a;

    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visibleLayerMask >> layer) & 1) == 0) continue;

        m_drawsScratch.clear();

        if (wrapX || wrapY)
        {
            const int spanX = chunkMaxX - chunkMinX + 1;
            const int spanY = chunkMaxY - chunkMinY + 1;
            m_drawsScratch.reserve(static_cast<size_t>(spanX) * static_cast<size_t>(spanY));
            for (int iy = 0; iy < spanY; ++iy)
            for (int ix = 0; ix < spanX; ++ix)
                m_drawsScratch.push_back({chunkMinX + ix, chunkMinY + iy,
                                          m_srcChunkXLut[ix], m_srcChunkYLut[iy]});
        }
        else
        {
            tm->QueryVisibleChunks(static_cast<int32_t>(layer),
                                   chunkMinX, chunkMinY, chunkMaxX, chunkMaxY, m_visibleScratch);
            m_drawsScratch.reserve(m_visibleScratch.size());
            for (const auto& entry : m_visibleScratch)
                m_drawsScratch.push_back({entry.chunkX, entry.chunkY, entry.chunkX, entry.chunkY});
        }

        for (const auto& d : m_drawsScratch)
        {
            const TileChunk* chunk = streamer->Get(static_cast<int32_t>(layer),
                                                   d.srcX, d.srcY);
            if (!chunk) continue;
            streamer->TouchLRU(static_cast<int32_t>(layer), d.srcX, d.srcY);

            const int chunkOriginX = d.drawX * cw * tw;
            const int chunkOriginY = d.drawY * ch * th;

            for (int ty = 0; ty < ch; ++ty)
            for (int tx = 0; tx < cw; ++tx)
            {
                const uint32_t gid = chunk->tileGids[ty * cw + tx];
                if (GidIndex(gid) == 0) continue;

                uint32_t localId = 0;
                size_t   tsIdx   = 0;
                const TilesetRef* tref = tm->ResolveTilesetWithIndex(gid, localId, tsIdx);
                if (!tref) continue;
                if (tsIdx >= cache.ready.size() || !cache.ready[tsIdx]) continue;

                Tileset* ts = cache.tilesets[tsIdx];

                int sx, sy, sw, sh;
                ts->GetTileRect(localId, sx, sy, sw, sh);

                // Tiled pixel coords of this tile's top-left, converted to
                // engine world meters (Y-up, centered on owner). The world
                // point we hand to WorldToScreen is the engine-top-left of
                // the tile — i.e. the corner with the *highest* engine Y,
                // which BlitScaled expects as its (destX, destY).
                const float tiledTileX = static_cast<float>(chunkOriginX + tx * tw) * invTilePPM;
                const float tiledTileY = static_cast<float>(chunkOriginY + ty * th) * invTilePPM;
                const float wx = originX + tiledTileX - originOffsetX;
                const float wy = originY + originOffsetY - tiledTileY;

                float fDestSX, fDestSY;
                ctx.camera->WorldToScreen(wx, wy, screenW, screenH, fDestSX, fDestSY);
                const int destSX = tc->pixelSnap
                    ? static_cast<int>(std::lround(fDestSX))
                    : static_cast<int>(fDestSX);
                const int destSY = tc->pixelSnap
                    ? static_cast<int>(std::lround(fDestSY))
                    : static_cast<int>(fDestSY);

                // Point the Source directly at this tile's slice of the atlas;
                // stride keeps QuadBlit walking the atlas's full row width so
                // adjacent tiles never bleed in. Chroma-key (when set on the
                // tileset) is honored by QuadBlit per-pixel without any copy.
                const QuadBlit::Source& base = cache.sources[tsIdx];
                QuadBlit::Source sub = base;
                sub.pixels = base.pixels + sy * base.stride + sx * base.bytesPerPixel;
                sub.width  = sw;
                sub.height = sh;
                // sub.stride stays at the atlas row width.

                // Source tile pixels -> world meters via tilePPM, then world
                // meters -> screen pixels via camera.PPM. Net scale is
                // (camera.PPM / tilePPM); when both match, source 1:1 to
                // screen. Tiled's flip flags go on the Source: a negative
                // size used to be passed instead, which BlitScaled rejects,
                // so every flipped tile silently vanished.
                const float scale = ctx.camera->GetPixelsPerMeter() * invTilePPM;
                const int destW = static_cast<int>(std::floor(static_cast<float>(sw) * scale));
                const int destH = static_cast<int>(std::floor(static_cast<float>(sh) * scale));
                sub.flipH = GidFlipH(gid);
                sub.flipV = GidFlipV(gid);
                sub.flipD = GidFlipD(gid);

                QuadBlit::BlitScaled(sub, ctx.buffer, screenW, screenH, ctx.format,
                                     destSX, destSY, destW, destH,
                                     tintR, tintG, tintB, tintA);
            }
        }
    }
}

} // namespace DekiTilemap

// Self-registration with autoAttach=true so DekiRenderingInit attaches the
// pass to the active Standard2DRenderer whenever the deki-tilemap package is
// loaded. The project's .rpipeline doesn't need to know about "tilemap"; it
// can still mention it explicitly to control ordering relative to other
// passes (e.g. clip2d) if needed.
namespace {
struct TilemapRenderPassRegistrar {
    TilemapRenderPassRegistrar() {
        RenderPassInfo info;
        info.factory    = []() -> RenderPass* { return new DekiTilemap::TilemapRenderPass(); };
        info.autoAttach = true;
        DekiRenderPassRegistry::Register(DekiTilemap::TilemapRenderPass::RegistryName, info);
    }
    // Unregister on DLL unload so the std::function factory (whose target
    // lives in this package's code) doesn't outlive the DLL and crash
    // deki-rendering's static-registry teardown.
    ~TilemapRenderPassRegistrar() {
        DekiRenderPassRegistry::Unregister(DekiTilemap::TilemapRenderPass::RegistryName);
    }
};
static TilemapRenderPassRegistrar s_tilemapPassRegistrar;
} // namespace
