#include "TilemapRenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// Reused scratch for tight per-tile copies (see comment at the blit site for
// why this exists). Grows on demand to fit the largest tile encountered.
thread_local std::vector<uint8_t> s_TileScratch;

// Build a Tileset Source descriptor referencing the atlas's pixel buffer.
// Returns false if the atlas isn't ready yet.
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
    outSrc.hasAlpha      = atlas->has_alpha;
    outSrc.alphaOffset   = atlas->has_alpha ? static_cast<uint8_t>(bpp - 1) : 0;
    outSrc.isRGB565      = (atlas->format == Texture2D::TextureFormat::RGB565 ||
                            atlas->format == Texture2D::TextureFormat::RGB565A8);
    outSrc.alphaRowSpans = nullptr;
    outSrc.ownsPixels    = false;
    return true;
}

} // namespace

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

    // Sprite-style centering: the GameObject's transform is the *center* of the
    // map for finite maps. For infinite maps we have no extents, so chunk (0,0)
    // top-left sits at the object position — author negative chunks to shift.
    // Y is also flipped here: Tiled stores rows top-to-bottom (Y+ = down) but
    // the engine is Y-up, so Tiled row 0 ends up at engine Y = +mapH/2.
    const float originX     = obj->GetWorldX();
    const float originY     = obj->GetWorldY();
    const float halfMapW    = tm->IsInfinite() ? 0.0f
                              : 0.5f * static_cast<float>(tm->MapWidth())  * static_cast<float>(tw);
    const float halfMapH    = tm->IsInfinite() ? 0.0f
                              : 0.5f * static_cast<float>(tm->MapHeight()) * static_cast<float>(th);

    // Camera visible rect, expressed in Tiled-data pixel coords (Y+ down).
    const float visW = ctx.camera->GetVisibleWidth(screenW);
    const float visH = ctx.camera->GetVisibleHeight(screenH);
    const float camX = ctx.camera->GetPositionX();
    const float camY = ctx.camera->GetPositionY();

    const float tiledMinX = (camX - originX) + halfMapW - visW * 0.5f;
    const float tiledMaxX = (camX - originX) + halfMapW + visW * 0.5f;
    // Top of screen = highest engine Y = lowest Tiled Y. Engine-Y high corresponds
    // to camY+visH/2, which maps to tiledY = halfMapH - (camY-originY+visH/2).
    const float tiledMinY = halfMapH - ((camY - originY) + visH * 0.5f);
    const float tiledMaxY = halfMapH - ((camY - originY) - visH * 0.5f);

    auto floorDiv = [](int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); };

    const int chunkMinX = floorDiv(static_cast<int>(std::floor(tiledMinX)) / tw, cw) - tc->chunk_padding;
    const int chunkMinY = floorDiv(static_cast<int>(std::floor(tiledMinY)) / th, ch) - tc->chunk_padding;
    const int chunkMaxX = floorDiv(static_cast<int>(std::floor(tiledMaxX)) / tw, cw) + tc->chunk_padding;
    const int chunkMaxY = floorDiv(static_cast<int>(std::floor(tiledMaxY)) / th, ch) + tc->chunk_padding;

    auto* streamer = tm->Streamer();
    if (!streamer) return;

    // Request + pump for every visible layer this frame.
    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visible_layer_mask >> layer) & 1) == 0) continue;
        streamer->RequestRect(static_cast<int32_t>(layer),
                              chunkMinX, chunkMinY, chunkMaxX, chunkMaxY);
    }
    streamer->Pump(kIOByteBudgetPerFrame);

    // Cache resolved tilesets (LoadByGuidAndType is moderately expensive).
    std::vector<Tileset*> tilesets;
    tilesets.reserve(tm->Tilesets().size());
    auto* mgr = Deki::AssetManager::Get();
    for (const auto& tref : tm->Tilesets())
    {
        Tileset* ts = mgr ? static_cast<Tileset*>(
            mgr->LoadByGuidAndType(tref.guid, Tileset::AssetTypeName)) : nullptr;
        tilesets.push_back(ts);
    }

    // Per-tileset Source descriptors (atlas may be null if not yet loaded).
    std::vector<QuadBlit::Source> atlasSrc(tilesets.size());
    std::vector<bool>             atlasReady(tilesets.size(), false);
    for (size_t i = 0; i < tilesets.size(); ++i)
        atlasReady[i] = MakeAtlasSource(tilesets[i], atlasSrc[i]);

    std::vector<ChunkIndexEntry> visible;

    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((tc->visible_layer_mask >> layer) & 1) == 0) continue;

        tm->QueryVisibleChunks(static_cast<int32_t>(layer),
                               chunkMinX, chunkMinY, chunkMaxX, chunkMaxY, visible);

        for (const auto& entry : visible)
        {
            const TileChunk* chunk = streamer->Get(static_cast<int32_t>(layer),
                                                   entry.chunkX, entry.chunkY);
            if (!chunk) continue;
            streamer->TouchLRU(static_cast<int32_t>(layer), entry.chunkX, entry.chunkY);

            const int chunkOriginX = entry.chunkX * cw * tw;
            const int chunkOriginY = entry.chunkY * ch * th;

            for (int ty = 0; ty < ch; ++ty)
            for (int tx = 0; tx < cw; ++tx)
            {
                const uint32_t gid = chunk->tileGids[ty * cw + tx];
                if (GidIndex(gid) == 0) continue;

                uint32_t localId = 0;
                const TilesetRef* tref = tm->ResolveTileset(gid, localId);
                if (!tref) continue;

                // Find the cached tileset slot for this ref.
                size_t tsIdx = 0;
                bool found = false;
                for (; tsIdx < tm->Tilesets().size(); ++tsIdx)
                {
                    if (&tm->Tilesets()[tsIdx] == tref) { found = true; break; }
                }
                if (!found || !atlasReady[tsIdx]) continue;

                Tileset* ts = tilesets[tsIdx];

                int sx, sy, sw, sh;
                ts->GetTileRect(localId, sx, sy, sw, sh);

                // Tiled pixel coords of this tile's top-left, then convert to
                // engine world coords (Y-up, centered on owner). The world point
                // we hand to WorldToScreen is the engine-top-left of the tile —
                // i.e. the corner with the *highest* engine Y, which BlitScaled
                // expects as its (destX, destY).
                const float tiledTileX = static_cast<float>(chunkOriginX + tx * tw);
                const float tiledTileY = static_cast<float>(chunkOriginY + ty * th);
                const float wx = originX + tiledTileX - halfMapW;
                const float wy = originY + halfMapH - tiledTileY;

                int destSX, destSY;
                ctx.camera->WorldToScreen(wx, wy, screenW, screenH, destSX, destSY);

                // Build a per-tile sub-source. QuadBlit::Source has no separate
                // stride, so pointing it at the atlas slice would make BlitScaled
                // read sw*bpp bytes per row from the atlas (which has a wider
                // stride) — adjacent tile rows in atlas memory would bleed in,
                // producing the horizontal-stripe artifact. Copy each tile into a
                // tight scratch buffer first so width == stride.
                const int      bpp         = atlasSrc[tsIdx].bytesPerPixel;
                const int32_t  atlasStride = atlasSrc[tsIdx].width * bpp;
                const int32_t  rowBytes    = sw * bpp;
                const size_t   needed      = static_cast<size_t>(sh) * static_cast<size_t>(rowBytes);
                if (s_TileScratch.size() < needed) s_TileScratch.resize(needed);
                const uint8_t* atlasBase = atlasSrc[tsIdx].pixels;
                for (int row = 0; row < sh; ++row)
                {
                    const uint8_t* srcRow = atlasBase + (sy + row) * atlasStride + sx * bpp;
                    std::memcpy(s_TileScratch.data() + row * rowBytes, srcRow,
                                static_cast<size_t>(rowBytes));
                }

                QuadBlit::Source sub = atlasSrc[tsIdx];
                sub.pixels = s_TileScratch.data();
                sub.width  = sw;
                sub.height = sh;

                // Apply tint from the component (white = no tint).
                const uint8_t tintR = tc->tint_color.r;
                const uint8_t tintG = tc->tint_color.g;
                const uint8_t tintB = tc->tint_color.b;
                const uint8_t tintA = tc->tint_color.a;

                // Match SpriteComponent's convention: blit at native source
                // pixel size (no GetZoom() multiplier). Camera zoom is already
                // baked into screenX/screenY by WorldToScreen. Multiplying here
                // too would render each tile at zoom*native pixels in the
                // buffer, which the prefab view then stretches to fit the
                // panel — a non-integer display ratio produces the
                // uneven-pixel "blurry" look the user reported.
                // Flip flags decode to negative size; BlitScaled treats those
                // as a horizontal/vertical flip.
                int destW = sw;
                int destH = sh;
                if (GidFlipH(gid)) destW = -destW;
                if (GidFlipV(gid)) destH = -destH;

                QuadBlit::BlitScaled(sub, ctx.buffer, screenW, screenH, ctx.format,
                                     destSX, destSY, destW, destH,
                                     tintR, tintG, tintB, tintA);
            }
        }
    }
}

} // namespace DekiTilemap

// Self-registration with autoAttach=true so DekiRenderingInit attaches the
// pass to the active Standard2DRenderer whenever the deki-tilemap module is
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
};
static TilemapRenderPassRegistrar s_tilemapPassRegistrar;
} // namespace
