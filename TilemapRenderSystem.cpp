#include "TilemapRenderSystem.h"

#include <algorithm>
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

// Build a Tileset Source descriptor referencing the atlas's pixel buffer.
// Returns false if the atlas isn't ready yet.
bool MakeAtlasSource(Tileset* ts, QuadBlit::Source& outSrc)
{
    if (!ts) return false;
    Sprite* atlas = ts->Atlas();
    if (!atlas || !atlas->data) return false;

    outSrc.pixels        = atlas->data;
    outSrc.width         = atlas->width;
    outSrc.height        = atlas->height;
    outSrc.bytesPerPixel = static_cast<int32_t>(atlas->bytes_per_pixel);
    outSrc.hasAlpha      = atlas->has_alpha;
    outSrc.alphaOffset   = atlas->has_alpha ? static_cast<uint8_t>(outSrc.bytesPerPixel - 1) : 0;
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

    // Camera visible rect in tile-pixels.
    const float visW = ctx.camera->GetVisibleWidth(screenW);
    const float visH = ctx.camera->GetVisibleHeight(screenH);
    const float camX = ctx.camera->GetPositionX();
    const float camY = ctx.camera->GetPositionY();

    const float worldMinX = camX - visW * 0.5f;
    const float worldMinY = camY - visH * 0.5f;
    const float worldMaxX = camX + visW * 0.5f;
    const float worldMaxY = camY + visH * 0.5f;

    const int tw = tm->TileWidth();
    const int th = tm->TileHeight();
    const int cw = tm->ChunkWidth();
    const int ch = tm->ChunkHeight();
    if (tw <= 0 || th <= 0 || cw <= 0 || ch <= 0) return;

    auto floorDiv = [](int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); };

    const int chunkMinX = floorDiv(static_cast<int>(worldMinX) / tw, cw) - tc->chunk_padding;
    const int chunkMinY = floorDiv(static_cast<int>(worldMinY) / th, ch) - tc->chunk_padding;
    const int chunkMaxX = floorDiv(static_cast<int>(worldMaxX) / tw, cw) + tc->chunk_padding;
    const int chunkMaxY = floorDiv(static_cast<int>(worldMaxY) / th, ch) + tc->chunk_padding;

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

                // World-space tile rect.
                const int wx = chunkOriginX + tx * tw;
                const int wy = chunkOriginY + ty * th;

                int destSX, destSY;
                ctx.camera->WorldToScreen(static_cast<float>(wx), static_cast<float>(wy),
                                          screenW, screenH, destSX, destSY);

                // Build a per-tile sub-source (the tileset atlas with srcRect baked in).
                // QuadBlit::Source doesn't carry a srcRect, so we point at the
                // atlas row/col origin and trust BlitScaled to copy sw x sh px.
                QuadBlit::Source sub = atlasSrc[tsIdx];
                sub.pixels = atlasSrc[tsIdx].pixels +
                             (sy * atlasSrc[tsIdx].width + sx) * atlasSrc[tsIdx].bytesPerPixel;
                sub.width  = sw;
                sub.height = sh;

                // Apply tint from the component (white = no tint).
                const uint8_t tintR = tc->tint_color.r;
                const uint8_t tintG = tc->tint_color.g;
                const uint8_t tintB = tc->tint_color.b;
                const uint8_t tintA = tc->tint_color.a;

                // Flip flags decode to negative scale; for v1 we issue an
                // unrotated BlitScaled (rotation/diagonal flips deferred).
                int destW = sw * ctx.camera->GetZoom();
                int destH = sh * ctx.camera->GetZoom();
                if (GidFlipH(gid)) destW = -destW;   // BlitScaled treats negative size as flip
                if (GidFlipV(gid)) destH = -destH;

                QuadBlit::BlitScaled(sub, ctx.buffer, screenW, screenH, ctx.format,
                                     destSX, destSY, destW, destH,
                                     tintR, tintG, tintB, tintA);
            }
        }
    }
}

} // namespace DekiTilemap

// Self-registration so the rendering pipeline can pick "tilemap" as a pass.
namespace {
struct TilemapRenderPassRegistrar {
    TilemapRenderPassRegistrar() {
        DekiRenderPassRegistry::Register(
            DekiTilemap::TilemapRenderPass::RegistryName,
            RenderPassInfo{ []() -> RenderPass* { return new DekiTilemap::TilemapRenderPass(); } });
    }
};
static TilemapRenderPassRegistrar s_tilemapPassRegistrar;
} // namespace
