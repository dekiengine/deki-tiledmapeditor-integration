#include "TilemapRenderSystem.h"
#include "PixelFormat.h"
#include "deki-2d/SpriteRowSpans.h"
#include <deki/assets/Texture2D.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <deki/Object.h>
#include <deki/Time.h>
#include <deki/LogSystem.h>
#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"
#include "deki-rendering/DekiRenderPassRegistry.h"
#include "deki-rendering/DirtyRegion.h"
#include "deki-rendering/Fingerprint.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-2d/Sprite.h"
#include "TileBlend.h"
#include "Tilemap.h"
#include "TilemapComponent.h"
#include "TilemapStreamer.h"
#include "Tileset.h"
#include <deki/assets/AssetManager.h>

namespace DekiTiledMap
{

namespace
{

// Animated tiles drawn so far (TilemapRenderPass::Animate): a view copy that
// drew one is not kept.
uint32_t s_AnimatedDraws = 0;

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
// A component's tint times a Tiled layer's tint (RGBA8, R low) and opacity.
void CombineTint(const Deki::Color& component, uint32_t layerTint, float layerOpacity, uint8_t& r, uint8_t& g,
                 uint8_t& b, uint8_t& a)
{
    auto times = [](uint32_t x, uint32_t y) { return static_cast<uint8_t>((x * y + 127u) / 255u); };
    const uint32_t opacity = static_cast<uint32_t>(std::lround(std::clamp(layerOpacity, 0.0f, 1.0f) * 255.0f));
    r = times(component.r, layerTint & 0xFFu);
    g = times(component.g, (layerTint >> 8) & 0xFFu);
    b = times(component.b, (layerTint >> 16) & 0xFFu);
    a = times(times(component.a, (layerTint >> 24) & 0xFFu), opacity);
}

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
    // The atlas's pixels stay as they are while it lives: a GPU uploads each
    // tile of it once.
    outSrc.revision = atlas->Revision();
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
    outSrc.filter = atlas->filter;  // the tileset image's

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
    cache.cellLeft.assign(refs.size(), 0);
    cache.cellTop.assign(refs.size(), 0);
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
        cache.animOf.clear();
        cache.anims.clear();
        cache.rowSpans.clear();
        cache.clearSpans.clear();
        cache.uniformRuns.clear();
        cache.binaryAlpha.clear();
        cache.opaqueTiles.clear();
        cache.opaqueRevisions.clear();
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
            cache.gidLut.assign(limit, TileLUT{ kUnresolved, 0, 0, 0, 0, -1, -1 });
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

// What an isometric chunk can cover, in map pixels. Its diamonds fill this box,
// and tile images can reach `extraX` past the sides and `extraTop` above.
struct IsoBox
{
    float left, top, right, bottom;
};

IsoBox IsoChunkBox(int baseColumn, int baseRow, int cw, int ch, int tw, int th, float extraX, float extraTop)
{
    const float halfW = static_cast<float>(tw) * 0.5f;
    const float halfH = static_cast<float>(th) * 0.5f;
    IsoBox box;
    box.left = static_cast<float>(baseColumn - baseRow - ch) * halfW - extraX;
    box.right = static_cast<float>(baseColumn + cw - baseRow) * halfW + extraX;
    box.top = static_cast<float>(baseColumn + baseRow) * halfH - extraTop;
    box.bottom = static_cast<float>(baseColumn + cw + baseRow + ch) * halfH;
    return box;
}
}  // namespace

uint32_t TilemapRenderPass::TilesetsOf(const Tilemap* tm, TilesetCache& cache, const TileChunk* chunk, uint32_t stamp)
{
    if (cache.chunkTilesetsStamp != stamp)
    {
        cache.chunkTilesets.clear();
        cache.chunkTilesetsStamp = stamp;
    }
    for (size_t i = 0; i < cache.chunkTilesets.size(); ++i)
    {
        if (cache.chunkTilesets.data()[i].chunk == chunk)
        {
            return cache.chunkTilesets.data()[i].mask;
        }
    }
    // An animated tile's frames come from its own tileset, so the tiles as
    // stored say it all.
    uint32_t mask = 0;
    const size_t count = static_cast<size_t>(chunk->width) * chunk->height;
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t index = GidIndex(chunk->tileGids[i]);
        if (index == 0)
        {
            continue;
        }
        int32_t tsIdx = 0, sx, sy, sw, sh, spans, opaque;
        if (ResolveTile(tm, cache, index, tsIdx, sx, sy, sw, sh, spans, opaque))
        {
            mask |= tsIdx < 32 ? (1u << tsIdx) : 0xFFFFFFFFu;
        }
        else if (cache.gidLimit == 0 || (index < cache.gidLimit && cache.gidLut[index].tsIdx == kUnresolved))
        {
            return 0xFFFFFFFFu;  // a tileset still loading: not known yet
        }
    }
    const size_t at = cache.chunkTilesets.size();
    if (cache.chunkTilesets.resize(at + 1))
    {
        cache.chunkTilesets.data()[at] = TilesetCache::ChunkTilesets{ chunk, mask };
    }
    return mask;
}

uint32_t TilemapRenderPass::Animate(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, uint32_t nowMs)
{
    if (cache.gidLimit == 0 || gidIndex == 0 || gidIndex >= cache.gidLimit)
    {
        return gidIndex;
    }
    if (cache.animOf.size() != cache.gidLimit)
    {
        cache.animOf.assign(cache.gidLimit, -2);
    }
    int32_t& found = cache.animOf[gidIndex];
    if (found == -2)
    {
        found = -1;
        uint32_t localId = 0;
        size_t tsIdx = 0;
        const TilesetRef* tref = tm->ResolveTilesetWithIndex(gidIndex, localId, tsIdx);
        const Tileset* ts = tref ? cache.tilesets[tsIdx] : nullptr;
        const DTileAnimation* anim = ts ? ts->GetAnimation(localId) : nullptr;
        const DTileAnimationFrame* frames = anim ? ts->GetAnimationFrames(*anim) : nullptr;
        if (frames && anim->frameCount > 0)
        {
            TilesetCache::TileAnimation a;
            for (uint32_t i = 0; i < anim->frameCount; ++i)
            {
                a.totalMs += frames[i].durationMs;
                a.gids.push_back(tref->firstGid + frames[i].localId);
                a.endsMs.push_back(a.totalMs);
            }
            if (a.totalMs > 0)
            {
                found = static_cast<int32_t>(cache.anims.size());
                cache.anims.push_back(std::move(a));
            }
        }
    }
    if (found < 0)
    {
        return gidIndex;
    }
    ++s_AnimatedDraws;  // a view copy must not keep one frame of it
    const TilesetCache::TileAnimation& a = cache.anims[static_cast<size_t>(found)];
    const uint32_t t = nowMs % a.totalMs;
    for (size_t i = 0; i < a.endsMs.size(); ++i)
    {
        if (t < a.endsMs[i])
        {
            return a.gids[i];
        }
    }
    return a.gids.back();
}

bool TilemapRenderPass::ResolveTile(const Tilemap* tm, TilesetCache& cache, uint32_t gidIndex, int32_t& outTsIdx,
                                    int32_t& outSx, int32_t& outSy, int32_t& outSw, int32_t& outSh, int32_t& outSpans,
                                    int32_t& outOpaque)
{
    outSpans = -1;
    outOpaque = -1;
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
                const Deki2D::Sprite* atlas = ts->Atlas();
                TileLUT entry{ static_cast<int32_t>(tsIdx), 0, 0, 0, 0, -1, -1 };
                ToStoredRect(atlas, sx, sy, sw, sh, entry.sx, entry.sy, entry.sw, entry.sh);
                // RGB565A8, the format of an atlas with see-through pixels:
                // the tile's spans are read off its rect in the atlas.
                if (atlas->hasAlpha && atlas->data && Deki::Texture2D::GetBytesPerPixel(atlas->format) == 3)
                {
                    const size_t spanCount = cache.rowSpans.size() + static_cast<size_t>(entry.sh) * 2;
                    if (!cache.rowSpans.resize(spanCount) || !cache.clearSpans.resize(spanCount) ||
                        !cache.uniformRuns.resize(spanCount / 2))
                    {
                        return false;  // no memory for its tables: not drawn, as when its atlas is not loaded
                    }
                    entry.spans = static_cast<int32_t>(spanCount) - entry.sh * 2;
                    const int32_t stride = atlas->width * 3;
                    const uint8_t* tilePixels = atlas->data + entry.sy * stride + entry.sx * 3;
                    Deki2D::BuildOpaqueRowSpans(tilePixels, entry.sw, entry.sh, stride,
                                                cache.rowSpans.data() + entry.spans);
                    Deki2D::BuildClearRowSpans(tilePixels, entry.sw, entry.sh, stride,
                                               cache.clearSpans.data() + entry.spans);
                    QuadBlit::FindUniformRuns(tilePixels, entry.sw, entry.sh, stride,
                                              cache.uniformRuns.data() + entry.spans / 2);

                    // Opaque in every row, and no colour key to cut holes: a
                    // copy of its colours, as stored, is the same picture.
                    bool opaque = !ts->HasTransparentColor();
                    for (int32_t y = 0; opaque && y < entry.sh; ++y)
                    {
                        const int16_t* span = cache.rowSpans.data() + entry.spans + y * 2;
                        opaque = span[0] == 0 && span[1] == entry.sw;
                    }
                    Deki::Buffer<uint16_t> copy;
                    if (opaque && copy.Allocate(static_cast<size_t>(entry.sw) * entry.sh, Deki::Memory::External))
                    {
                        for (int32_t y = 0; y < entry.sh; ++y)
                        {
                            for (int32_t x = 0; x < entry.sw; ++x)
                            {
                                std::memcpy(copy.Data() + y * entry.sw + x, tilePixels + y * stride + x * 3, 2);
                            }
                        }
                        entry.opaque = static_cast<int32_t>(cache.opaqueTiles.size());
                        cache.opaqueTiles.push_back(std::move(copy));
                        cache.opaqueRevisions.push_back(QuadBlit::NextRevision());
                    }
                }
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
        outSpans = e.spans;
        outOpaque = e.opaque;
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

bool TilemapRenderPass::WantsObject(Deki::Object* obj) const
{
    return obj && obj->GetComponent<TilemapComponent>() != nullptr;
}

void TilemapRenderPass::AddChanges(Deki::Object* obj, const DekiRendering::RenderContext& ctx,
                                   DekiRendering::DirtyRegion& changed)
{
    (void)ctx;
    auto* tc = obj ? obj->GetComponent<TilemapComponent>() : nullptr;
    if (!tc)
    {
        return;
    }
    // Everything that decides the map's pixels besides the camera (a camera
    // that moves redraws everything anyway), field by field.
    Tilemap* tm = tc->tilemap.Get();
    TilemapStreamer* streamer = tm ? tm->Streamer() : nullptr;
    size_t ready = 0;
    if (tm)
    {
        const TilesetCache& cache = GetCache(tm);
        ready = static_cast<size_t>(std::count(cache.ready.begin(), cache.ready.end(), true));
    }
    DekiRendering::Fingerprint f;
    f.Add(static_cast<const void*>(tm));
    f.Add(obj->GetWorldX());
    f.Add(obj->GetWorldY());
    f.Add(static_cast<uint32_t>(tc->visibleLayerMask));
    f.Add(static_cast<uint32_t>(tc->tintColor.r) | (static_cast<uint32_t>(tc->tintColor.g) << 8) |
          (static_cast<uint32_t>(tc->tintColor.b) << 16) | (static_cast<uint32_t>(tc->tintColor.a) << 24));
    f.Add(tc->pixelsPerMeter);
    f.Add(static_cast<uint32_t>(tc->loopX) | (static_cast<uint32_t>(tc->loopY) << 1));
    f.Add(static_cast<uint32_t>(tc->wrapPeriodX));
    f.Add(static_cast<uint32_t>(tc->wrapPeriodY));
    f.Add(static_cast<uint32_t>(tc->chunkPadding));
    f.Add(static_cast<uint32_t>(ready));
    f.Add(streamer ? streamer->Changes() : 0u);
    if (tm)
    {
        const TilesetCache& cache = GetCache(tm);
        const uint32_t now = Deki::Time::GetTime();
        for (const TilesetCache::TileAnimation& a : cache.anims)
        {
            const uint32_t t = now % a.totalMs;
            f.Add(static_cast<uint32_t>(std::upper_bound(a.endsMs.begin(), a.endsMs.end(), t) - a.endsMs.begin()));
        }
    }
    const Deki::AssetManager* assets = Deki::AssetManager::Get();
    const uint64_t epoch = assets ? assets->GetEpoch() : 0;
    f.Add(static_cast<uint32_t>(epoch));
    f.Add(static_cast<uint32_t>(epoch >> 32));
    const uint64_t h = f.Value();

    // A camera that only scrolled moves the map's pixels by whole pixels when
    // the map itself sits on whole pixels at 1:1, with no layer moving apart
    // from it (parallax, an offset of a fraction) and, on an isometric map,
    // half tiles of whole pixels. Otherwise it is drawn again.
    if ((ctx.scrollX != 0 || ctx.scrollY != 0) && tm)
    {
        auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1.0e-3f; };
        const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
        float anchorX = 0.0f, anchorY = 0.0f;
        tm->GetAnchor(anchorX, anchorY);
        bool follows = ctx.cam.ppm == tilePPM && whole(obj->GetWorldX() * tilePPM) &&
                       whole(obj->GetWorldY() * tilePPM) && whole(anchorX) && whole(anchorY) &&
                       whole(tm->BoundsLeft()) &&
                       (!tm->IsIsometric() || (tm->TileWidth() % 2 == 0 && tm->TileHeight() % 2 == 0));
        for (const DLayerLook& look : tm->Layers())
        {
            follows = follows && look.parallaxX == 1.0f && look.parallaxY == 1.0f && whole(look.offsetX) &&
                      whole(look.offsetY);
        }
        if (!follows)
        {
            changed.SetFull();
        }
    }

    Drawn& drawn = m_Drawn[tc];
    const bool loading = streamer && streamer->Pending() > 0;
    if (drawn.seen == 0 || drawn.signature != h || loading)
    {
        changed.SetFull();
    }
    drawn.signature = h;
    drawn.seen = m_ChangesCall;
}

void TilemapRenderPass::FinishChanges(const DekiRendering::RenderContext& ctx, DekiRendering::DirtyRegion& changed)
{
    (void)ctx;
    // A map no longer drawn leaves its tiles behind until the view is redrawn.
    for (auto it = m_Drawn.begin(); it != m_Drawn.end();)
    {
        if (it->second.seen != m_ChangesCall)
        {
            changed.SetFull();
            it = m_Drawn.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (++m_ChangesCall == 0)
    {
        m_ChangesCall = 1;
    }
}

void TilemapRenderPass::Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    auto* tc = obj ? obj->GetComponent<TilemapComponent>() : nullptr;
    Tilemap* tm = tc ? tc->tilemap.Get() : nullptr;
    if (!tm || !ctx.camera)
    {
        return;
    }
    // A later part of a frame whose first part came from the copies.
    if (ctx.contentFrame != 0 && DrawPartFromCopies(obj, tc, ctx))
    {
        return;
    }
    FramePlan* plan = ctx.contentFrame != 0 ? &StartFramePlan(obj, tc, ctx) : nullptr;
    const std::vector<DLayerLook>& looks = tm->Layers();
    if (looks.empty())
    {
        // A map baked before layers kept their look: every layer, plain.
        DrawLayers(obj, ctx, 0xFFFFFFFFu, 0.0f, 0.0f, 0xFFFFFFFFu, 1.0f);
        if (plan)
        {
            plan->fromCopies = false;
        }
        return;
    }

    // Where a layer sits: Tiled moves it by its offset, and a layer with
    // parallax also by (1 - factor) times the view's centre plus the map's
    // Parallax Origin (Tiled's MapScene::parallaxOffset). Map pixels, Y down;
    // then world meters.
    const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
    float anchorX = 0.0f, anchorY = 0.0f;
    tm->GetAnchor(anchorX, anchorY);
    // Tiled measures from the top-left of the map's bounds, which on an
    // isometric map is half its height in tiles left of the first tile.
    const float boundsLeft = tm->BoundsLeft();
    const float viewX = (ctx.camera->GetPositionX() - obj->GetWorldX()) * tilePPM + anchorX + boundsLeft;
    const float viewY = anchorY - (ctx.camera->GetPositionY() - obj->GetWorldY()) * tilePPM;
    auto shiftOf = [&](const DLayerLook& look, float& outX, float& outY)
    {
        const float px = look.offsetX + (1.0f - look.parallaxX) * (viewX + tm->ParallaxOriginX());
        const float py = look.offsetY + (1.0f - look.parallaxY) * (viewY + tm->ParallaxOriginY());
        outX = px / tilePPM;
        outY = -py / tilePPM;
    };

    // Bottom to top, as in Tiled. Neighbouring tile layers that look the same
    // are drawn together; object layers are the spawner's.
    const size_t end = std::min<size_t>(looks.size(), tc->endLayer);
    size_t i = tc->firstLayer;
    while (i < end)
    {
        const DLayerLook& look = looks[i];
        float shiftX = 0.0f, shiftY = 0.0f;
        shiftOf(look, shiftX, shiftY);
        if (look.kind == static_cast<uint8_t>(DLayerKind::Image))
        {
            if ((look.flags & kLayerHidden) == 0)
            {
                DrawImageLayer(obj, ctx, look, shiftX, shiftY);
                if (plan)
                {
                    plan->fromCopies = false;
                }
            }
            ++i;
            continue;
        }
        if (look.kind != static_cast<uint8_t>(DLayerKind::Tile))
        {
            ++i;
            continue;
        }
        uint32_t mask = 0;
        size_t j = i;
        while (j < end && looks[j].kind == static_cast<uint8_t>(DLayerKind::Tile) && looks[j].opacity == look.opacity &&
               looks[j].tint == look.tint && looks[j].offsetX == look.offsetX && looks[j].offsetY == look.offsetY &&
               looks[j].parallaxX == look.parallaxX && looks[j].parallaxY == look.parallaxY &&
               looks[j].blendMode == look.blendMode)
        {
            if (looks[j].index < 32)
            {
                mask |= 1u << looks[j].index;
            }
            ++j;
        }
        // A layer that moves with the camera (parallax) is drawn every time.
        const bool moves = look.parallaxX != 1.0f || look.parallaxY != 1.0f;
        FramePlan::Group group;
        group.layerMask = mask;
        if (moves || !DrawFromCopy(obj, ctx, mask, shiftX, shiftY, look.tint, look.opacity, look.blendMode, &group))
        {
            DrawLayers(obj, ctx, mask, shiftX, shiftY, look.tint, look.opacity, look.blendMode);
            group.copied = false;
        }
        if (plan)
        {
            // m_FramePlans may have grown under a nested draw: found again.
            plan = FindFramePlan(obj);
            if (plan && group.copied)
            {
                plan->groups.push_back(group);
            }
            else if (plan)
            {
                plan->fromCopies = false;
            }
        }
        i = j;
    }
}

void TilemapRenderPass::DrawImageLayer(Deki::Object* obj, DekiRendering::RenderContext& ctx, const DLayerLook& look,
                                       float shiftX, float shiftY)
{
    auto* tc = obj->GetComponent<TilemapComponent>();
    Tilemap* tm = tc->tilemap.Get();
    auto* mgr = Deki::AssetManager::Get();
    if (!mgr || look.imageGuid[0] == '\0' || !ctx.buffer || !ctx.cam.valid)
    {
        return;
    }
    auto* image = static_cast<Deki2D::Sprite*>(mgr->LoadByGuidAndType(look.imageGuid, Deki2D::Sprite::kAssetTypeName));
    if (!image || !image->data || image->width <= 0 || image->height <= 0)
    {
        return;
    }

    uint8_t tintR = 255, tintG = 255, tintB = 255, tintA = 255;
    CombineTint(tc->tintColor, look.tint, look.opacity, tintR, tintG, tintB, tintA);
    if (tintA == 0)
    {
        return;
    }

    QuadBlit::Source source = QuadBlit::MakeSource(image->data, image->width, image->height,
                                                   QuadBlit::PixelLayout::FromTexture(image->format, image->hasAlpha));
    source.filter = image->filter;

    // Its top-left is the map's top-left (Tiled pixels 0, 0), moved by the
    // layer's offset and parallax, which shiftX and shiftY hold.
    const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
    float anchorX = 0.0f, anchorY = 0.0f;
    tm->GetAnchor(anchorX, anchorY);
    const DekiRendering::FrameCamera& cam = ctx.cam;
    // On an isometric map Tiled's 0, 0 is the top-left of the map's bounds,
    // half the map's height in tiles across to the left of the first tile's
    // top corner, where these pixels start.
    const float boundsLeft = -tm->BoundsLeft();
    float left = 0.0f, top = 0.0f;
    cam.WorldToScreen(obj->GetWorldX() + shiftX + (boundsLeft - anchorX) / tilePPM,
                      obj->GetWorldY() + shiftY + anchorY / tilePPM, left, top);
    const float scale = cam.ppm / tilePPM;
    const int destW = static_cast<int>(std::floor(static_cast<float>(image->width) * scale));
    const int destH = static_cast<int>(std::floor(static_cast<float>(image->height) * scale));
    if (destW <= 0 || destH <= 0)
    {
        return;
    }
    const int x0 = static_cast<int>(std::floor(left));
    const int y0 = static_cast<int>(std::floor(top));

    // A repeated image fills the view across, down, or both, as in Tiled.
    const bool repeatX = (look.flags & kLayerRepeatX) != 0;
    const bool repeatY = (look.flags & kLayerRepeatY) != 0;
    const int startX = repeatX ? x0 - ((x0 % destW) + destW) % destW : x0;
    const int startY = repeatY ? y0 - ((y0 % destH) + destH) % destH : y0;
    const int endX = repeatX ? ctx.width : startX + 1;
    const int endY = repeatY ? ctx.height : startY + 1;
    for (int y = startY; y < endY; y += destH)
    {
        for (int x = startX; x < endX; x += destW)
        {
            if (look.blendMode != 0)
            {
                BlendBlit(source, ctx, x, y, destW, destH, tintR, tintG, tintB, tintA, look.blendMode,
                          !image->hasAlpha && !source.hasChromaKey);
            }
            else
            {
                QuadBlit::BlitScaled(source, ctx.buffer, ctx.width, ctx.height, ctx.format, x, y, destW, destH, tintR,
                                     tintG, tintB, tintA);
            }
        }
    }
}

namespace
{
// A row of the frame as 0xAARRGGBB and back. 565 channels are read with
// their low bits filled in (23 of 31 is 189, not 184): Dodge and Burn turn a
// few levels into many.
template <Deki::ColorFormat F>
void ReadFrameRow(const uint8_t* frame, size_t first, int count, uint32_t* out)
{
    for (int i = 0; i < count; ++i)
    {
        uint8_t r, g, b, a;
        DekiPixel::ReadDstPixel<F>(frame, first + static_cast<size_t>(i), r, g, b, a);
        if constexpr (F == Deki::ColorFormat::RGB565 || F == Deki::ColorFormat::RGB565A8)
        {
            r = static_cast<uint8_t>(r | (r >> 5));
            g = static_cast<uint8_t>(g | (g >> 6));
            b = static_cast<uint8_t>(b | (b >> 5));
        }
        out[i] =
            (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    }
}

template <Deki::ColorFormat F>
void WriteFrameRow(uint8_t* frame, size_t first, int count, const uint32_t* in)
{
    for (int i = 0; i < count; ++i)
    {
        const uint32_t v = in[i];
        DekiPixel::WriteDstPixel<F>(frame, first + static_cast<size_t>(i), static_cast<uint8_t>(v >> 16),
                                    static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v),
                                    static_cast<uint8_t>(v >> 24));
    }
}

using BlendRowFn = void (*)(uint32_t*, const uint32_t*, const uint8_t*, int);

BlendRowFn BlendRowFor(DBlendMode mode)
{
    switch (mode)
    {
        case DBlendMode::Normal: return &TileBlend::BlendRow<DBlendMode::Normal>;
        case DBlendMode::Add: return &TileBlend::BlendRow<DBlendMode::Add>;
        case DBlendMode::Multiply: return &TileBlend::BlendRow<DBlendMode::Multiply>;
        case DBlendMode::Screen: return &TileBlend::BlendRow<DBlendMode::Screen>;
        case DBlendMode::Overlay: return &TileBlend::BlendRow<DBlendMode::Overlay>;
        case DBlendMode::Darken: return &TileBlend::BlendRow<DBlendMode::Darken>;
        case DBlendMode::Lighten: return &TileBlend::BlendRow<DBlendMode::Lighten>;
        case DBlendMode::ColorDodge: return &TileBlend::BlendRow<DBlendMode::ColorDodge>;
        case DBlendMode::ColorBurn: return &TileBlend::BlendRow<DBlendMode::ColorBurn>;
        case DBlendMode::HardLight: return &TileBlend::BlendRow<DBlendMode::HardLight>;
        case DBlendMode::SoftLight: return &TileBlend::BlendRow<DBlendMode::SoftLight>;
        case DBlendMode::Difference: return &TileBlend::BlendRow<DBlendMode::Difference>;
        case DBlendMode::Exclusion: return &TileBlend::BlendRow<DBlendMode::Exclusion>;
    }
    return &TileBlend::BlendRow<DBlendMode::Normal>;
}
}  // namespace

void TilemapRenderPass::BlendBlit(const QuadBlit::Source& source, DekiRendering::RenderContext& ctx, int destX,
                                  int destY, int destW, int destH, uint8_t tintR, uint8_t tintG, uint8_t tintB,
                                  uint8_t tintA, uint8_t blendMode, bool coversRect)
{
    const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
    const int left = std::max<int32_t>({ 0, clip.left, destX });
    const int top = std::max<int32_t>({ 0, clip.top, destY });
    const int right = std::min<int32_t>({ ctx.width, clip.right, destX + destW });
    const int bottom = std::min<int32_t>({ ctx.height, clip.bottom, destY + destH });
    if (left >= right || top >= bottom || !ctx.buffer)
    {
        return;
    }
    // A picture that fills its rect with opaque pixels writes every pixel of
    // it: nothing to clear first, and no alpha to find.
    const bool opaque = coversRect && tintA == 255;
    // A see-through tile on a 565 frame is drawn once on clear RGB565A8 (its
    // colours are 565 anyway); an opaque one once over black, as 8-bit.
    const bool frame565 =
        !opaque && (ctx.format == Deki::ColorFormat::RGB565 || ctx.format == Deki::ColorFormat::RGB565A8);
    const size_t pixels = static_cast<size_t>(ctx.width) * static_cast<size_t>(ctx.height);
    const bool ready = frame565 ? m_Blend565.Count() == pixels * 3
                                : m_BlendOnBlack.Count() == pixels && (opaque || m_BlendOnWhite.Count() == pixels);
    if (!ready || m_BlendRow.size() != static_cast<size_t>(ctx.width))
    {
        bool ok = true;
        if (frame565)
        {
            ok = m_Blend565.Count() == pixels * 3 || m_Blend565.Allocate(pixels * 3, Deki::Memory::External);
        }
        else
        {
            ok =
                (m_BlendOnBlack.Count() == pixels || m_BlendOnBlack.Allocate(pixels, Deki::Memory::External)) &&
                (opaque || m_BlendOnWhite.Count() == pixels || m_BlendOnWhite.Allocate(pixels, Deki::Memory::External));
        }
        if (!ok)
        {
            m_Blend565.Reset();
            m_BlendOnBlack.Reset();
            m_BlendOnWhite.Reset();
            DEKI_LOG_WARNING("Tilemap: no memory to draw a layer's blend mode; it is drawn Normal");
            QuadBlit::BlitScaled(source, ctx.buffer, ctx.width, ctx.height, ctx.format, destX, destY, destW, destH,
                                 tintR, tintG, tintB, tintA);
            return;
        }
        m_BlendRow.resize(static_cast<size_t>(ctx.width));
        m_BlendDrawn.resize(static_cast<size_t>(ctx.width));
        m_BlendAlpha.resize(static_cast<size_t>(ctx.width));
    }
    const int count = right - left;
    const size_t width = static_cast<size_t>(ctx.width);

    // Draw the tile on its own: once on clear RGB565A8 for a 565 frame, else
    // over black and, unless it is opaque, over white.
    uint8_t* drawn565 = frame565 ? m_Blend565.Data() : nullptr;
    uint32_t* onBlack = frame565 ? nullptr : m_BlendOnBlack.Data();
    uint32_t* onWhite = frame565 || opaque ? nullptr : m_BlendOnWhite.Data();
    for (int y = top; !opaque && y < bottom; ++y)
    {
        const size_t first = static_cast<size_t>(y) * width + static_cast<size_t>(left);
        if (frame565)
        {
            std::memset(drawn565 + first * 3, 0, static_cast<size_t>(count) * 3);
        }
        else
        {
            std::fill(onBlack + first, onBlack + first + count, 0xFF000000u);
            std::fill(onWhite + first, onWhite + first + count, 0xFFFFFFFFu);
        }
    }
    if (frame565)
    {
        QuadBlit::BlitScaled(source, drawn565, ctx.width, ctx.height, Deki::ColorFormat::RGB565A8, destX, destY, destW,
                             destH, tintR, tintG, tintB, tintA);
    }
    else
    {
        if (onWhite)
        {
            QuadBlit::BlitScaled(source, reinterpret_cast<uint8_t*>(onWhite), ctx.width, ctx.height,
                                 Deki::ColorFormat::ARGB8888, destX, destY, destW, destH, tintR, tintG, tintB, tintA);
        }
        QuadBlit::BlitScaled(source, reinterpret_cast<uint8_t*>(onBlack), ctx.width, ctx.height,
                             Deki::ColorFormat::ARGB8888, destX, destY, destW, destH, tintR, tintG, tintB, tintA);
    }

    const BlendRowFn blendRow = BlendRowFor(static_cast<DBlendMode>(blendMode));
    uint32_t* frameRow = m_BlendRow.data();
    uint8_t* alphaRow = m_BlendAlpha.data();
    for (int y = top; y < bottom; ++y)
    {
        const size_t first = static_cast<size_t>(y) * width + static_cast<size_t>(left);
        const uint32_t* drawnRow = nullptr;
        const uint8_t* alpha = nullptr;
        if (frame565)
        {
            // Colours over black, read back with their low bits filled in.
            uint32_t* out = m_BlendDrawn.data();
            const uint8_t* p = drawn565 + first * 3;
            for (int i = 0; i < count; ++i, p += 3)
            {
                uint8_t r, g, b;
                DekiPixel::UnpackRGB565(DekiPixel::Load565(p), r, g, b);
                out[i] = (static_cast<uint32_t>(r | (r >> 5)) << 16) | (static_cast<uint32_t>(g | (g >> 6)) << 8) |
                         static_cast<uint32_t>(b | (b >> 5));
                alphaRow[i] = p[2];
            }
            drawnRow = out;
            alpha = opaque ? nullptr : alphaRow;
        }
        else
        {
            drawnRow = onBlack + first;
            if (onWhite)
            {
                TileBlend::AlphaFromBlackAndWhite(onBlack + first, onWhite + first, alphaRow, count);
                alpha = alphaRow;
            }
        }
        switch (ctx.format)
        {
            case Deki::ColorFormat::RGB565:
                ReadFrameRow<Deki::ColorFormat::RGB565>(ctx.buffer, first, count, frameRow);
                blendRow(frameRow, drawnRow, alpha, count);
                WriteFrameRow<Deki::ColorFormat::RGB565>(ctx.buffer, first, count, frameRow);
                break;
            case Deki::ColorFormat::RGB888:
                ReadFrameRow<Deki::ColorFormat::RGB888>(ctx.buffer, first, count, frameRow);
                blendRow(frameRow, drawnRow, alpha, count);
                WriteFrameRow<Deki::ColorFormat::RGB888>(ctx.buffer, first, count, frameRow);
                break;
            case Deki::ColorFormat::ARGB8888:
                ReadFrameRow<Deki::ColorFormat::ARGB8888>(ctx.buffer, first, count, frameRow);
                blendRow(frameRow, drawnRow, alpha, count);
                WriteFrameRow<Deki::ColorFormat::ARGB8888>(ctx.buffer, first, count, frameRow);
                break;
            case Deki::ColorFormat::RGB565A8:
                ReadFrameRow<Deki::ColorFormat::RGB565A8>(ctx.buffer, first, count, frameRow);
                blendRow(frameRow, drawnRow, alpha, count);
                WriteFrameRow<Deki::ColorFormat::RGB565A8>(ctx.buffer, first, count, frameRow);
                break;
        }
    }
}

void TilemapRenderPass::DrawLayers(Deki::Object* obj, DekiRendering::RenderContext& ctx, uint32_t layerMask,
                                   float shiftX, float shiftY, uint32_t layerTint, float layerOpacity,
                                   uint8_t blendMode)
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
    const float originX = obj->GetWorldX() + shiftX;
    const float originY = obj->GetWorldY() + shiftY;
    float originOffsetX = 0.0f;
    float originOffsetY = 0.0f;
    tm->GetAnchor(originOffsetX, originOffsetY);
    originOffsetX *= invTilePPM;
    originOffsetY *= invTilePPM;

    // The camera's visible rect in map pixels (Y down), to pick chunks. Sizes
    // are meters, converted with tilePPM. Visible size = screen / ppm
    // (DekiRendering::CameraComponent::GetVisibleWidth). The position comes
    // from the camera, not the snapshot, which may be pixel-snapped.
    const float visW = (ctx.cam.ppm > 0.0f) ? (static_cast<float>(screenW) / ctx.cam.ppm) : 0.0f;
    const float visH = (ctx.cam.ppm > 0.0f) ? (static_cast<float>(screenH) / ctx.cam.ppm) : 0.0f;
    // Drawing a view copy, the centre of the copy instead (ViewCopy).
    const float camX = m_CopyCentre ? m_CopyCentreX : ctx.camera->GetPositionX();
    const float camY = m_CopyCentre ? m_CopyCentreY : ctx.camera->GetPositionY();

    // Map pixels (meters * tilePPM) for the chunk math.
    float tiledMinX = ((camX - originX) + originOffsetX - visW * 0.5f) * tilePPM;
    float tiledMaxX = ((camX - originX) + originOffsetX + visW * 0.5f) * tilePPM;
    float tiledMinY = (originOffsetY - ((camY - originY) + visH * 0.5f)) * tilePPM;
    float tiledMaxY = (originOffsetY - ((camY - originY) - visH * 0.5f)) * tilePPM;

    // A frame redrawn in parts (Redraw Changes), or a part of one, reaches
    // only the chunks its clip does: the chunks are walked, asked for and set
    // up only there. For a whole frame the clip is the view.
    {
        const QuadBlit::ClipRect part = QuadBlit::GetCurrentClipRect();
        const float l = static_cast<float>(std::max<int32_t>(0, part.left));
        const float t = static_cast<float>(std::max<int32_t>(0, part.top));
        const float r = static_cast<float>(std::min<int32_t>(screenW, part.right));
        const float b = static_cast<float>(std::min<int32_t>(screenH, part.bottom));
        const DekiRendering::FrameCamera& view = ctx.cam;
        if (l < r && t < b && view.ppm > 0.0f)
        {
            const float worldL = view.camX + (l - view.halfW) / view.ppm;
            const float worldR = view.camX + (r - view.halfW) / view.ppm;
            const float worldT = view.camY - (t - view.halfH) / view.ppm;
            const float worldB = view.camY - (b - view.halfH) / view.ppm;
            tiledMinX = std::max(tiledMinX, (worldL - originX + originOffsetX) * tilePPM);
            tiledMaxX = std::min(tiledMaxX, (worldR - originX + originOffsetX) * tilePPM);
            tiledMinY = std::max(tiledMinY, (originOffsetY - (worldT - originY)) * tilePPM);
            tiledMaxY = std::min(tiledMaxY, (originOffsetY - (worldB - originY)) * tilePPM);
        }
    }

    auto floorDiv = [](int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); };

    auto* streamer = tm->Streamer();
    if (!streamer)
    {
        return;
    }

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

    const bool isometric = tm->IsIsometric();

    // How far an isometric tile's image can reach past its diamond, in map
    // pixels. An image sits on the bottom point of its diamond, so a tall one
    // rises above it and a wide one overhangs both sides.
    float isoExtraX = 0.0f;
    float isoExtraTop = 0.0f;
    // The same for any map: how far past its cell a tile's image can reach,
    // with its Tile Offset.
    float extraX = 0.0f, extraTop = 0.0f;
    for (const Tileset* ts : cache.tilesets)
    {
        if (ts)
        {
            float left, top, w, h;
            ts->DrawnTileRect(tw, th, left, top, w, h);
            const float offX =
                std::fabs(static_cast<float>(ts->TileOffsetX())) * (w / std::max(1, static_cast<int>(ts->TileWidth())));
            const float offY = std::fabs(static_cast<float>(ts->TileOffsetY())) *
                               (h / std::max(1, static_cast<int>(ts->TileHeight())));
            isoExtraX = std::max(isoExtraX, w - static_cast<float>(tw));
            isoExtraTop = std::max(isoExtraTop, h - static_cast<float>(th));
            extraX = std::max(extraX, std::max(0.0f, w - static_cast<float>(tw)) + offX);
            extraTop = std::max(extraTop, std::max(0.0f, h - static_cast<float>(th)) + offY);
        }
    }
    if (!isometric)
    {
        isoExtraX = 0.0f;
        isoExtraTop = 0.0f;
    }
    const bool staggered = tm->IsStaggered();
    const Tilemap::StaggerGrid& grid = tm->Stagger();

    int chunkMinX, chunkMinY, chunkMaxX, chunkMaxY;
    if (isometric)
    {
        // The view is a rotated rectangle in tile space. Take the box round
        // its corners; the boxes tested below drop the chunks it does not touch.
        const float corners[4][2] = { { tiledMinX - isoExtraX, tiledMinY },
                                      { tiledMaxX + isoExtraX, tiledMinY },
                                      { tiledMinX - isoExtraX, tiledMaxY + isoExtraTop },
                                      { tiledMaxX + isoExtraX, tiledMaxY + isoExtraTop } };
        float minColumn = 0.0f, maxColumn = 0.0f, minRow = 0.0f, maxRow = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            float column = 0.0f, row = 0.0f;
            tm->PixelsToTile(corners[i][0], corners[i][1], column, row);
            minColumn = i == 0 ? column : std::min(minColumn, column);
            maxColumn = i == 0 ? column : std::max(maxColumn, column);
            minRow = i == 0 ? row : std::min(minRow, row);
            maxRow = i == 0 ? row : std::max(maxRow, row);
        }
        chunkMinX = static_cast<int>(std::floor(minColumn / static_cast<float>(cw))) - tc->chunkPadding;
        chunkMinY = static_cast<int>(std::floor(minRow / static_cast<float>(ch))) - tc->chunkPadding;
        chunkMaxX = static_cast<int>(std::floor(maxColumn / static_cast<float>(cw))) + tc->chunkPadding;
        chunkMaxY = static_cast<int>(std::floor(maxRow / static_cast<float>(ch))) + tc->chunkPadding;
    }
    else if (staggered)
    {
        // Columns and rows step by less than a tile and overlap; take a tile
        // more each way, and what a big tile or its offset can reach.
        const float stepX = static_cast<float>(grid.staggerX ? grid.columnWidth : grid.tileWidth + grid.sideLengthX);
        const float stepY = static_cast<float>(grid.staggerX ? grid.tileHeight + grid.sideLengthY : grid.rowHeight);
        const int minColumn =
            static_cast<int>(std::floor((tiledMinX - extraX - static_cast<float>(grid.tileWidth)) / stepX)) - 1;
        const int maxColumn = static_cast<int>(std::floor((tiledMaxX + extraX) / stepX)) + 1;
        const int minRow =
            static_cast<int>(std::floor((tiledMinY - extraTop - static_cast<float>(grid.tileHeight)) / stepY)) - 1;
        const int maxRow =
            static_cast<int>(std::floor((tiledMaxY + extraTop + static_cast<float>(grid.tileHeight)) / stepY)) + 1;
        chunkMinX = floorDiv(minColumn, cw) - tc->chunkPadding;
        chunkMinY = floorDiv(minRow, ch) - tc->chunkPadding;
        chunkMaxX = floorDiv(maxColumn, cw) + tc->chunkPadding;
        chunkMaxY = floorDiv(maxRow, ch) + tc->chunkPadding;
    }
    else
    {
        chunkMinX = floorDiv(static_cast<int>(std::floor(tiledMinX)) / tw, cw) - tc->chunkPadding;
        chunkMinY = floorDiv(static_cast<int>(std::floor(tiledMinY)) / th, ch) - tc->chunkPadding;
        chunkMaxX = floorDiv(static_cast<int>(std::floor(tiledMaxX)) / tw, cw) + tc->chunkPadding;
        chunkMaxY = floorDiv(static_cast<int>(std::floor(tiledMaxY)) / th, ch) + tc->chunkPadding;
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

    // Layers hidden in Tiled stay hidden, whatever the mask says.
    const uint32_t drawnLayers = static_cast<uint32_t>(tc->visibleLayerMask) & ~tm->HiddenLayerMask() & layerMask;

    // Asked for already this frame, within a clip that holds this one?
    bool requested = false;
    {
        const QuadBlit::ClipRect part = QuadBlit::GetCurrentClipRect();
        const int32_t l = std::max<int32_t>(0, part.left), t = std::max<int32_t>(0, part.top);
        const int32_t r = std::min<int32_t>(screenW, part.right), b = std::min<int32_t>(screenH, part.bottom);
        size_t keep = 0;
        for (size_t i = 0; i < m_Requested.size(); ++i)
        {
            const Requested& q = m_Requested.data()[i];
            if (q.frame != ctx.contentFrame)
            {
                continue;  // an older frame's: dropped
            }
            m_Requested.data()[keep++] = q;
            requested =
                requested || (ctx.contentFrame != 0 && q.tc == tc && q.layers == drawnLayers && q.shiftX == shiftX &&
                              q.shiftY == shiftY && q.left <= l && q.top <= t && q.right >= r && q.bottom >= b);
        }
        m_Requested.resize(keep);
        if (!requested && ctx.contentFrame != 0 && m_Requested.resize(keep + 1))
        {
            m_Requested.data()[keep] = Requested{ tc, drawnLayers, shiftX, shiftY, ctx.contentFrame, l, t, r, b };
        }
    }

    // Request and load chunks for every visible layer.
    for (uint32_t layer = 0; layer < tm->LayerCount() && !requested; ++layer)
    {
        if (((drawnLayers >> layer) & 1) == 0)
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
        else if (isometric)
        {
            // Only the chunks whose box reaches the view, grown by the
            // padding, so the rotated corners of the range stay unloaded.
            const float padX = static_cast<float>(tc->chunkPadding * cw * tw) * 0.5f;
            const float padY = static_cast<float>(tc->chunkPadding * ch * th) * 0.5f;
            for (int cy = chunkMinY; cy <= chunkMaxY; ++cy)
            {
                for (int cx = chunkMinX; cx <= chunkMaxX; ++cx)
                {
                    const IsoBox box = IsoChunkBox(cx * cw, cy * ch, cw, ch, tw, th, isoExtraX, isoExtraTop);
                    if (box.right >= tiledMinX - padX && box.left <= tiledMaxX + padX &&
                        box.bottom >= tiledMinY - padY && box.top <= tiledMaxY + padY)
                    {
                        streamer->RequestRect(static_cast<int32_t>(layer), cx, cy, cx, cy);
                    }
                }
            }
        }
        else
        {
            streamer->RequestRect(static_cast<int32_t>(layer), chunkMinX, chunkMinY, chunkMaxX, chunkMaxY);
        }
    }
    if (!requested)
    {
        streamer->Pump(kIOByteBudgetPerFrame);
    }

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

    // The component's tint, times the layer's tint and opacity from Tiled.
    uint8_t tintR = 255, tintG = 255, tintB = 255, tintA = 255;
    CombineTint(tc->tintColor, layerTint, layerOpacity, tintR, tintG, tintB, tintA);
    if (tintA == 0)
    {
        return;
    }
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
        // Its own size, or with Tile Render Size the map's grid; Tiled scales
        // the Tile Offset with the tile.
        float left = 0.0f, top = 0.0f, w = 0.0f, h = 0.0f;
        ts->DrawnTileRect(tw, th, left, top, w, h);
        const float scaleX = w / static_cast<float>(std::max(1, static_cast<int>(ts->TileWidth())));
        const float scaleY = h / static_cast<float>(std::max(1, static_cast<int>(ts->TileHeight())));
        const float below = -(top + h);  // gap between the image's bottom and the cell's
        cache.destW[i] = static_cast<int32_t>(std::floor(w * scale));
        cache.destH[i] = static_cast<int32_t>(std::floor(h * scale));
        cache.cellLeft[i] = static_cast<int32_t>(std::floor(left * scale)) +
                            static_cast<int32_t>(std::floor(static_cast<float>(ts->TileOffsetX()) * scaleX * scale));
        cache.cellTop[i] = -cache.destH[i] - static_cast<int32_t>(std::floor(below * scale)) +
                           static_cast<int32_t>(std::floor(static_cast<float>(ts->TileOffsetY()) * scaleY * scale));
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

    // Draws one tile of an atlas. The tile's slice is what the Source points
    // at; the stride keeps QuadBlit on the atlas's full row width, so
    // neighbouring tiles never bleed in. QuadBlit applies the tileset's chroma
    // key per pixel. Tiled's flip flags go on the Source; BlitScaled rejects a
    // negative size.
    auto blitTile = [&](uint32_t gid, int32_t tsIdx, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t spans,
                        int32_t opaque, int destSX, int destSY, int destW, int destH)
    {
        const QuadBlit::Source& base = cache.sources[tsIdx];
        QuadBlit::Source& sub = cache.scratch[tsIdx];
        sub.pixels = base.pixels + sy * base.stride + sx * base.bytesPerPixel;
        sub.width = sw;  // stored pixels; the destination keeps the tileset's size
        sub.height = sh;
        sub.flipH = GidFlipH(gid);
        sub.flipV = GidFlipV(gid);
        sub.flipD = GidFlipD(gid);
        sub.alphaRowSpans = spans >= 0 ? cache.rowSpans.data() + spans : nullptr;
        sub.clearRowSpans = spans >= 0 ? cache.clearSpans.data() + spans : nullptr;
        sub.uniformRuns = spans >= 0 ? cache.uniformRuns.data() + spans / 2 : nullptr;

        if (blendMode != 0 && opaque < 0)
        {
            // A tileset with no alpha and no colour key fills its rect.
            BlendBlit(sub, ctx, destSX, destSY, destW, destH, tintR, tintG, tintB, tintA, blendMode,
                      !sub.hasAlpha && !sub.hasChromaKey);
            return;
        }
        if (opaque >= 0)
        {
            QuadBlit::Source plain =
                QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(cache.opaqueTiles[opaque].Data()), sw, sh,
                                     QuadBlit::PixelLayout::RGB565());
            plain.flipH = sub.flipH;
            plain.flipV = sub.flipV;
            plain.flipD = sub.flipD;
            plain.revision = cache.opaqueRevisions[opaque];
            plain.filter = sub.filter;
            if (blendMode != 0)
            {
                // The opaque copy fills its rect: one draw for the blend.
                BlendBlit(plain, ctx, destSX, destSY, destW, destH, tintR, tintG, tintB, tintA, blendMode, true);
                return;
            }
            QuadBlit::BlitScaled(plain, ctx.buffer, screenW, screenH, ctx.format, destSX, destSY, destW, destH, tintR,
                                 tintG, tintB, tintA);
        }
        else
        {
            QuadBlit::BlitScaled(sub, ctx.buffer, screenW, screenH, ctx.format, destSX, destSY, destW, destH, tintR,
                                 tintG, tintB, tintA);
        }
    };

    const bool roundToPixel = pixelSnap || cam.snapStep > 0;
    // Animated tiles all keep the same clock, as in Tiled.
    const uint32_t nowMs = Deki::Time::GetTime();
    const int isoHalfTile = static_cast<int>(std::floor(static_cast<float>(tw) * scale)) / 2;

    // How far a tile's image reaches from its cell, over every tileset that
    // is ready, in screen pixels: across from the cell's left (or an
    // isometric cell's middle less half a tile), and up and down from its
    // bottom. A cell whose reach misses the clip draws nothing, so the loops
    // below skip it, and a strip of the frame walks only its own rows.
    int32_t cellMinLeft = 0, cellMaxRight = 0, cellMinTop = 0, cellMaxBottom = 0;
    bool anyTilesetReady = false;
    for (size_t t = 0; t < cache.destW.size(); ++t)
    {
        if (!cache.ready[t])
        {
            continue;
        }
        const int32_t left = cache.cellLeft[t], right = left + cache.destW[t];
        const int32_t top = cache.cellTop[t], bottom = top + cache.destH[t];
        cellMinLeft = anyTilesetReady ? std::min(cellMinLeft, left) : left;
        cellMaxRight = anyTilesetReady ? std::max(cellMaxRight, right) : right;
        cellMinTop = anyTilesetReady ? std::min(cellMinTop, top) : top;
        cellMaxBottom = anyTilesetReady ? std::max(cellMaxBottom, bottom) : bottom;
        anyTilesetReady = true;
    }
    // The same over only the tilesets `mask` names. False when none is ready.
    const uint32_t chunkStamp = streamer->Changes() + 1;
    auto reachOf = [&](uint32_t mask, int32_t& minLeft, int32_t& maxRight, int32_t& minTop, int32_t& maxBottom)
    {
        bool any = false;
        for (size_t t = 0; t < cache.destW.size(); ++t)
        {
            if (!cache.ready[t] || (t < 32 && ((mask >> t) & 1u) == 0))
            {
                continue;
            }
            const int32_t left = cache.cellLeft[t], right = left + cache.destW[t];
            const int32_t top = cache.cellTop[t], bottom = top + cache.destH[t];
            minLeft = any ? std::min(minLeft, left) : left;
            maxRight = any ? std::max(maxRight, right) : right;
            minTop = any ? std::min(minTop, top) : top;
            maxBottom = any ? std::max(maxBottom, bottom) : bottom;
            any = true;
        }
        return any;
    };

    // Rounded down for negative numbers too.
    auto floorHalf = [](int v) { return v >= 0 ? v / 2 : -((1 - v) / 2); };

    // Screen positions of columns and rows (or diagonals), shared by every
    // layer of this call: the range covers every chunk drawn below.
    {
        const int colLo = chunkMinX * cw, colHi = (chunkMaxX + 1) * cw;
        const int rowLo = chunkMinY * ch, rowHi = (chunkMaxY + 1) * ch;
        if (isometric)
        {
            m_AxisCache[0].Reset(colLo - (rowHi - 1), colHi - 1 - rowLo);  // column minus row
            m_AxisCache[1].Reset(colLo + rowLo, colHi - 1 + rowHi - 1);    // column plus row
        }
        else
        {
            m_AxisCache[0].Reset(colLo, colHi);
            m_AxisCache[1].Reset(rowLo, rowHi);
            m_AxisCache[2].Reset(colLo, colHi);  // staggered: the shifted kind
            m_AxisCache[3].Reset(rowLo, rowHi);
        }
    }

    for (uint32_t layer = 0; layer < tm->LayerCount(); ++layer)
    {
        if (((drawnLayers >> layer) & 1) == 0)
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

        if (isometric)
        {
            // The chunks in view, and the range of the two diagonals that
            // decide where a tile lands: its column minus its row gives the
            // screen X, its column plus its row the screen Y.
            m_IsoChunks.clear();
            int minDiff = 0, maxDiff = 0, minSum = 0, maxSum = 0;
            for (const auto& d : m_DrawsScratch)
            {
                const int baseColumn = d.drawX * cw;
                const int baseRow = d.drawY * ch;
                const IsoBox box = IsoChunkBox(baseColumn, baseRow, cw, ch, tw, th, isoExtraX, isoExtraTop);
                float left, top, right, bottom;
                cam.WorldToScreen(originX + box.left * invTilePPM - originOffsetX,
                                  originY + originOffsetY - box.top * invTilePPM, left, top);
                cam.WorldToScreen(originX + box.right * invTilePPM - originOffsetX,
                                  originY + originOffsetY - box.bottom * invTilePPM, right, bottom);
                if (right + 2.0f < static_cast<float>(clipL) || left - 2.0f > static_cast<float>(clipR) ||
                    bottom + 2.0f < static_cast<float>(clipT) || top - 2.0f > static_cast<float>(clipB))
                {
                    continue;
                }
                const TileChunk* chunk =
                    streamer->GetAndTouch(static_cast<int32_t>(layer), d.srcX, d.srcY, m_FrameSerial);
                if (!chunk)
                {
                    NoteMissing(streamer, layer, d.srcX, d.srcY);
                    continue;
                }

                const int diffLow = baseColumn - (baseRow + ch - 1);
                const int diffHigh = baseColumn + cw - 1 - baseRow;
                const int sumLow = baseColumn + baseRow;
                const int sumHigh = sumLow + cw + ch - 2;
                minDiff = m_IsoChunks.empty() ? diffLow : std::min(minDiff, diffLow);
                maxDiff = m_IsoChunks.empty() ? diffHigh : std::max(maxDiff, diffHigh);
                minSum = m_IsoChunks.empty() ? sumLow : std::min(minSum, sumLow);
                maxSum = m_IsoChunks.empty() ? sumHigh : std::max(maxSum, sumHigh);
                m_IsoChunks.push_back({ chunk, baseColumn, baseRow });
            }
            if (m_IsoChunks.empty())
            {
                continue;
            }

            // Each diagonal's screen position, once rather than once a tile:
            // the middle of the diamond across, and its bottom point down.
            m_ChunkColX.resize(static_cast<size_t>(maxDiff - minDiff + 1));
            m_ChunkRowY.resize(static_cast<size_t>(maxSum - minSum + 1));
            auto diffX = [&](int diff)
            {
                float screenX, unusedY;
                cam.WorldToScreen(originX + static_cast<float>(diff * tw) * 0.5f * invTilePPM - originOffsetX, originY,
                                  screenX, unusedY);
                if (cam.snapStep > 0)
                {
                    screenX = cam.SnapX(screenX);
                }
                return roundToPixel ? static_cast<int>(std::lround(screenX)) : static_cast<int>(screenX);
            };
            auto sumY = [&](int sum)
            {
                float unusedX, screenY;
                cam.WorldToScreen(originX,
                                  originY + originOffsetY - static_cast<float>((sum + 2) * th) * 0.5f * invTilePPM,
                                  unusedX, screenY);
                if (cam.snapStep > 0)
                {
                    screenY = cam.SnapY(screenY);
                }
                return roundToPixel ? static_cast<int>(std::lround(screenY)) : static_cast<int>(screenY);
            };
            for (int diff = minDiff; diff <= maxDiff; ++diff)
            {
                m_ChunkColX[diff - minDiff] = m_AxisCache[0].Get(diff, diffX);
            }
            for (int sum = minSum; sum <= maxSum; ++sum)
            {
                m_ChunkRowY[sum - minSum] = m_AxisCache[1].Get(sum, sumY);
            }

            // How far this layer's own tiles reach from their cells.
            uint32_t layerTilesets = 0;
            for (const IsoChunk& c : m_IsoChunks)
            {
                layerTilesets |= TilesetsOf(tm, cache, c.chunk, chunkStamp);
            }
            int32_t layerMinLeft = 0, layerMaxRight = 0, layerMinTop = 0, layerMaxBottom = 0;
            if (!reachOf(layerTilesets, layerMinLeft, layerMaxRight, layerMinTop, layerMaxBottom))
            {
                continue;  // no tile of it can be drawn
            }

            // The columns whose tiles can reach the clip: screen X grows with
            // column minus row.
            int visibleDiffLo = minDiff, visibleDiffHi = maxDiff;
            while (visibleDiffLo <= maxDiff &&
                   m_ChunkColX[visibleDiffLo - minDiff] - isoHalfTile + layerMaxRight <= clipL)
            {
                ++visibleDiffLo;
            }
            while (visibleDiffHi >= visibleDiffLo &&
                   m_ChunkColX[visibleDiffHi - minDiff] - isoHalfTile + layerMinLeft >= clipR)
            {
                --visibleDiffHi;
            }

            // Back to front, one diagonal at a time and across every chunk, so
            // a tall tile covers what is behind it even over a chunk's edge.
            for (int sum = minSum; sum <= maxSum; ++sum)
            {
                // A diagonal whose tiles cannot reach the clip draws nothing.
                const int rowBottom = m_ChunkRowY[sum - minSum];
                if (rowBottom + layerMinTop >= clipB || rowBottom + layerMaxBottom <= clipT)
                {
                    continue;
                }
                for (const IsoChunk& c : m_IsoChunks)
                {
                    const int t = sum - (c.baseColumn + c.baseRow);
                    if (t < 0 || t > cw + ch - 2)
                    {
                        continue;
                    }
                    // A tile's column minus row is `across` less twice its row:
                    // only the rows that put it in the visible columns.
                    const int across = c.baseColumn - c.baseRow + t;
                    const int lastY = std::min({ ch - 1, t, floorHalf(across - visibleDiffLo) });
                    for (int ty = std::max({ 0, t - (cw - 1), -floorHalf(visibleDiffHi - across) }); ty <= lastY; ++ty)
                    {
                        const int tx = t - ty;
                        const uint32_t gid = c.chunk->tileGids[ty * cw + tx];
                        if (GidIndex(gid) == 0)
                        {
                            continue;
                        }
                        int32_t tsIdx, sx, sy, sw, sh, spans, opaque;
                        if (!ResolveTile(tm, cache, Animate(tm, cache, GidIndex(gid), nowMs), tsIdx, sx, sy, sw, sh,
                                         spans, opaque) ||
                            !cache.ready[tsIdx])
                        {
                            continue;
                        }

                        const int destW = cache.destW[tsIdx];
                        const int destH = cache.destH[tsIdx];
                        const int diff = (c.baseColumn + tx) - (c.baseRow + ty);
                        // Tiled lines a wide tile up with the left corner of its square.
                        const int destSX = m_ChunkColX[diff - minDiff] - isoHalfTile + cache.cellLeft[tsIdx];
                        const int destSY = m_ChunkRowY[sum - minSum] + cache.cellTop[tsIdx];
                        if (destW <= 0 || destH <= 0 || destSX >= clipR || destSX + destW <= clipL || destSY >= clipB ||
                            destSY + destH <= clipT)
                        {
                            continue;
                        }
                        blitTile(gid, tsIdx, sx, sy, sw, sh, spans, opaque, destSX, destSY, destW, destH);
                    }
                }
            }
            continue;
        }

        if (staggered)
        {
            // The chunks in view, and the columns and rows they cover.
            m_IsoChunks.clear();
            int minColumn = 0, maxColumn = 0, minRow = 0, maxRow = 0;
            for (const auto& d : m_DrawsScratch)
            {
                const TileChunk* chunk =
                    streamer->GetAndTouch(static_cast<int32_t>(layer), d.srcX, d.srcY, m_FrameSerial);
                if (!chunk)
                {
                    NoteMissing(streamer, layer, d.srcX, d.srcY);
                    continue;
                }
                const int baseColumn = d.drawX * cw;
                const int baseRow = d.drawY * ch;
                minColumn = m_IsoChunks.empty() ? baseColumn : std::min(minColumn, baseColumn);
                maxColumn = m_IsoChunks.empty() ? baseColumn + cw - 1 : std::max(maxColumn, baseColumn + cw - 1);
                minRow = m_IsoChunks.empty() ? baseRow : std::min(minRow, baseRow);
                maxRow = m_IsoChunks.empty() ? baseRow + ch - 1 : std::max(maxRow, baseRow + ch - 1);
                m_IsoChunks.push_back({ chunk, baseColumn, baseRow });
            }
            if (m_IsoChunks.empty())
            {
                continue;
            }

            // Each column's screen X and each row's screen Y (a cell's bottom),
            // twice: where the shifted ones go too. Rows are shifted across
            // (columns down, with Stagger Axis X).
            const int columns = maxColumn - minColumn + 1;
            const int rows = maxRow - minRow + 1;
            m_ChunkColX.resize(static_cast<size_t>(columns) * 2);
            m_ChunkRowY.resize(static_cast<size_t>(rows) * 2);
            auto toScreenX = [&](float mapX)
            {
                float screenX, unusedY;
                cam.WorldToScreen(originX + mapX * invTilePPM - originOffsetX, originY, screenX, unusedY);
                if (cam.snapStep > 0)
                {
                    screenX = cam.SnapX(screenX);
                }
                return roundToPixel ? static_cast<int>(std::lround(screenX)) : static_cast<int>(screenX);
            };
            auto toScreenY = [&](float mapY)
            {
                float unusedX, screenY;
                cam.WorldToScreen(originX, originY + originOffsetY - mapY * invTilePPM, unusedX, screenY);
                if (cam.snapStep > 0)
                {
                    screenY = cam.SnapY(screenY);
                }
                return roundToPixel ? static_cast<int>(std::lround(screenY)) : static_cast<int>(screenY);
            };
            for (int i = 0; i < columns; ++i)
            {
                const int column = minColumn + i;
                for (int shifted = 0; shifted < 2; ++shifted)
                {
                    auto columnX = [&](int c)
                    {
                        int32_t x = 0, y = 0;
                        // A row whose shift matches: even or odd as needed.
                        const int row = grid.Shifted(0) == (shifted != 0) ? 0 : 1;
                        grid.TileCorner(c, grid.staggerX ? 0 : row, x, y);
                        return toScreenX(static_cast<float>(x));
                    };
                    m_ChunkColX[static_cast<size_t>(shifted) * columns + i] =
                        m_AxisCache[shifted ? 2 : 0].Get(column, columnX);
                }
            }
            for (int i = 0; i < rows; ++i)
            {
                const int row = minRow + i;
                for (int shifted = 0; shifted < 2; ++shifted)
                {
                    int32_t x = 0, y = 0;
                    const int column = grid.Shifted(0) == (shifted != 0) ? 0 : 1;
                    grid.TileCorner(grid.staggerX ? column : 0, row, x, y);
                    m_ChunkRowY[static_cast<size_t>(shifted) * rows + i] =
                        toScreenY(static_cast<float>(y + grid.tileHeight));
                }
            }

            // Top to bottom as Tiled draws them: row by row, and with Stagger
            // Axis X the columns that are not shifted down before those that
            // are. Left to right across every chunk in each.
            // How far this layer's own tiles reach from their cells.
            uint32_t layerTilesets = 0;
            for (const IsoChunk& c : m_IsoChunks)
            {
                layerTilesets |= TilesetsOf(tm, cache, c.chunk, chunkStamp);
            }
            int32_t layerMinLeft = 0, layerMaxRight = 0, layerMinTop = 0, layerMaxBottom = 0;
            if (!reachOf(layerTilesets, layerMinLeft, layerMaxRight, layerMinTop, layerMaxBottom))
            {
                continue;
            }
            // How far above and below its cell's bottom any tile reaches, so a
            // row wholly outside the clip (another strip) is skipped whole.
            int reachUp = 0, reachDown = 0;
            for (size_t t = 0; t < cache.cellTop.size(); ++t)
            {
                if (cache.ready[t] && (t >= 32 || ((layerTilesets >> t) & 1u)))
                {
                    reachUp = std::max<int32_t>(reachUp, -cache.cellTop[t]);
                    reachDown = std::max<int32_t>(reachDown, cache.cellTop[t] + cache.destH[t]);
                }
            }
            // The columns whose tiles can reach the clip, shifted or not:
            // screen X grows with the column either way.
            int visibleColumnLo = minColumn, visibleColumnHi = maxColumn;
            auto columnX = [&](int column, int shifted)
            { return m_ChunkColX[static_cast<size_t>(shifted) * columns + (column - minColumn)]; };
            while (visibleColumnLo <= maxColumn &&
                   std::max(columnX(visibleColumnLo, 0), columnX(visibleColumnLo, 1)) + layerMaxRight <= clipL)
            {
                ++visibleColumnLo;
            }
            while (visibleColumnHi >= visibleColumnLo &&
                   std::min(columnX(visibleColumnHi, 0), columnX(visibleColumnHi, 1)) + layerMinLeft >= clipR)
            {
                --visibleColumnHi;
            }
            const int passes = grid.staggerX ? 2 : 1;
            const int step = grid.staggerX ? 2 : 1;
            for (int row = minRow; row <= maxRow; ++row)
            {
                const int rowY0 = m_ChunkRowY[static_cast<size_t>(row - minRow)];
                const int rowY1 = m_ChunkRowY[static_cast<size_t>(rows) + (row - minRow)];
                if (std::min(rowY0, rowY1) - reachUp >= clipB || std::max(rowY0, rowY1) + reachDown <= clipT)
                {
                    continue;
                }
                for (int pass = 0; pass < passes; ++pass)
                {
                    for (const IsoChunk& c : m_IsoChunks)
                    {
                        const int ty = row - c.baseRow;
                        if (ty < 0 || ty >= ch)
                        {
                            continue;
                        }
                        // With Stagger Axis X a pass takes every other column:
                        // start on the first of its kind.
                        int firstTx = 0;
                        if (grid.staggerX && (grid.Shifted(c.baseColumn) ? 1 : 0) != pass)
                        {
                            firstTx = 1;
                        }
                        // From the first visible column of this pass's kind.
                        while (c.baseColumn + firstTx < visibleColumnLo)
                        {
                            firstTx += step;
                        }
                        const int txEnd = std::min(cw, visibleColumnHi - c.baseColumn + 1);
                        for (int tx = firstTx; tx < txEnd; tx += step)
                        {
                            const int column = c.baseColumn + tx;
                            const bool columnShifted = grid.staggerX && pass == 1;
                            const uint32_t gid = c.chunk->tileGids[ty * cw + tx];
                            if (GidIndex(gid) == 0)
                            {
                                continue;
                            }
                            int32_t tsIdx, sx, sy, sw, sh, spans, opaque;
                            if (!ResolveTile(tm, cache, Animate(tm, cache, GidIndex(gid), nowMs), tsIdx, sx, sy, sw, sh,
                                             spans, opaque) ||
                                !cache.ready[tsIdx])
                            {
                                continue;
                            }
                            const int destW = cache.destW[tsIdx];
                            const int destH = cache.destH[tsIdx];
                            const bool rowShifted = !grid.staggerX && grid.Shifted(row);
                            const int destSX =
                                m_ChunkColX[static_cast<size_t>(rowShifted ? 1 : 0) * columns + (column - minColumn)] +
                                cache.cellLeft[tsIdx];
                            const int destSY =
                                m_ChunkRowY[static_cast<size_t>(columnShifted ? 1 : 0) * rows + (row - minRow)] +
                                cache.cellTop[tsIdx];
                            if (destW <= 0 || destH <= 0 || destSX >= clipR || destSX + destW <= clipL ||
                                destSY >= clipB || destSY + destH <= clipT)
                            {
                                continue;
                            }
                            blitTile(gid, tsIdx, sx, sy, sw, sh, spans, opaque, destSX, destSY, destW, destH);
                        }
                    }
                }
            }
            continue;
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
                NoteMissing(streamer, layer, d.srcX, d.srcY);
                continue;
            }

            // Each column's and each row's screen position, once for the
            // chunk rather than once a tile: WorldToScreen works x and y out
            // apart, so these are the very calls each tile made, with the
            // same results.
            //
            // A tile's corner is its top-left in Tiled pixels, converted to
            // world meters (Y up, centred on the owner): in engine terms the
            // corner with the highest Y, which BlitScaled takes as (destX,
            // destY).
            m_ChunkColX.resize(static_cast<size_t>(cw));
            m_ChunkRowY.resize(static_cast<size_t>(ch) + 1);  // one past the last row: its bottom edge
            auto columnX = [&](int column)
            {
                const float tiledTileX = static_cast<float>(column * tw) * invTilePPM;
                const float wx = originX + tiledTileX - originOffsetX;
                float fDestSX, unusedY;
                cam.WorldToScreen(wx, originY, fDestSX, unusedY);
                if (cam.snapStep > 0)  // project Pixel Perfect: the art-pixel grid
                {
                    fDestSX = cam.SnapX(fDestSX);
                }
                return roundToPixel ? static_cast<int>(std::lround(fDestSX)) : static_cast<int>(fDestSX);
            };
            auto rowY = [&](int row)
            {
                const float tiledTileY = static_cast<float>(row * th) * invTilePPM;
                const float wy = originY + originOffsetY - tiledTileY;
                float unusedX, fDestSY;
                cam.WorldToScreen(originX, wy, unusedX, fDestSY);
                if (cam.snapStep > 0)
                {
                    fDestSY = cam.SnapY(fDestSY);
                }
                return roundToPixel ? static_cast<int>(std::lround(fDestSY)) : static_cast<int>(fDestSY);
            };
            for (int tx = 0; tx < cw; ++tx)
            {
                m_ChunkColX[tx] = m_AxisCache[0].Get(d.drawX * cw + tx, columnX);
            }
            for (int ty = 0; ty <= ch; ++ty)
            {
                m_ChunkRowY[ty] = m_AxisCache[1].Get(d.drawY * ch + ty, rowY);
            }

            // Only the rows and columns whose tiles (this chunk's own) can
            // reach the clip.
            int32_t chunkMinLeft = 0, chunkMaxRight = 0, chunkMinTop = 0, chunkMaxBottom = 0;
            if (!reachOf(TilesetsOf(tm, cache, chunk, chunkStamp), chunkMinLeft, chunkMaxRight, chunkMinTop,
                         chunkMaxBottom))
            {
                continue;
            }
            int tyFirst = 0, tyEnd = ch, txFirst = 0, txEnd = cw;
            while (tyFirst < ch && m_ChunkRowY[tyFirst + 1] + chunkMaxBottom <= clipT)
            {
                ++tyFirst;
            }
            while (tyEnd > tyFirst && m_ChunkRowY[tyEnd] + chunkMinTop >= clipB)
            {
                --tyEnd;
            }
            while (txFirst < cw && m_ChunkColX[txFirst] + chunkMaxRight <= clipL)
            {
                ++txFirst;
            }
            while (txEnd > txFirst && m_ChunkColX[txEnd - 1] + chunkMinLeft >= clipR)
            {
                --txEnd;
            }
            for (int ty = tyFirst; ty < tyEnd; ++ty)
            {
                for (int tx = txFirst; tx < txEnd; ++tx)
                {
                    const uint32_t gid = chunk->tileGids[ty * cw + tx];
                    if (GidIndex(gid) == 0)
                    {
                        continue;  // an empty cell
                    }
                    int32_t tsIdx, sx, sy, sw, sh, spans, opaque;
                    if (!ResolveTile(tm, cache, Animate(tm, cache, GidIndex(gid), nowMs), tsIdx, sx, sy, sw, sh, spans,
                                     opaque))
                    {
                        continue;
                    }
                    if (!cache.ready[tsIdx])
                    {
                        continue;
                    }

                    const int destW = cache.destW[tsIdx];
                    const int destH = cache.destH[tsIdx];
                    const int destSX = m_ChunkColX[tx] + cache.cellLeft[tsIdx];
                    // Tiled stands a tile on the bottom of its cell.
                    const int destSY = m_ChunkRowY[ty + 1] + cache.cellTop[tsIdx];
                    if (destW <= 0 || destH <= 0 || destSX >= clipR || destSX + destW <= clipL || destSY >= clipB ||
                        destSY + destH <= clipT)
                    {
                        continue;  // BlitScaled would clip this to nothing
                    }

                    blitTile(gid, tsIdx, sx, sy, sw, sh, spans, opaque, destSX, destSY, destW, destH);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The view copy (TilemapRenderPass::ViewCopy)
// ---------------------------------------------------------------------------

namespace
{
// Pixels the copy keeps past each edge of the view, and how close the view
// may come to an edge of what is drawn before a band is drawn out past it, to
// the margin beyond the view. A band is drawn a piece a frame (it is cut where
// the copy wraps), so no frame draws more than one, and a camera moving a
// pixel a frame starts a band about every 16 frames.
constexpr int32_t kCopyMargin = 16;
constexpr int32_t kCopyLookAhead = 8;
// Kept no further than this from where it was first drawn: float positions
// stay exact well within it, and past it the copy starts again.
constexpr int32_t kCopyMaxOffset = 1 << 14;
// Copies kept at once, the least recently used dropped first.
constexpr size_t kMaxViewCopies = 4;

int32_t FloorDivInt(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Where world pixel 0 lands on screen, across and down, through a camera: a
// point's screen x is its world x in pixels less the first, and its screen y
// the second less its world y in pixels (world Y is up).
double OffsetAcross(const DekiRendering::FrameCamera& c)
{
    return static_cast<double>(c.camX) * c.ppm - c.halfW;
}
double OffsetDown(const DekiRendering::FrameCamera& c)
{
    return static_cast<double>(c.camY) * c.ppm + c.halfH;
}

bool OnWholePixels(double v)
{
    return std::fabs(v - std::round(v)) <= 1.0e-3;
}

bool WholeInt(double v, int32_t& out)
{
    const double r = std::round(v);
    if (std::fabs(v - r) > 1.0e-3 || std::fabs(r) > kCopyMaxOffset)
    {
        return false;
    }
    out = static_cast<int32_t>(r);
    return true;
}

bool Inside(const Deki::Rect& inner, const Deki::Rect& outer)
{
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

// `r` cut where the copy wraps: pieces that each lie in one repeat of a w x h
// copy, with which repeat (kx, ky). Up to four.
int SplitWrapped(const Deki::Rect& r, int32_t w, int32_t h, Deki::Rect* out, int32_t* kx, int32_t* ky)
{
    int n = 0;
    for (int32_t y = r.top; y < r.bottom;)
    {
        const int32_t ry = FloorDivInt(y, h);
        const int32_t yEnd = std::min(r.bottom, (ry + 1) * h);
        for (int32_t x = r.left; x < r.right;)
        {
            const int32_t rx = FloorDivInt(x, w);
            const int32_t xEnd = std::min(r.right, (rx + 1) * w);
            if (n < 4)
            {
                out[n] = Deki::Rect{ x, y, xEnd, yEnd };
                kx[n] = rx;
                ky[n] = ry;
                ++n;
            }
            x = xEnd;
        }
        y = yEnd;
    }
    return n;
}

// Whether every pixel of an atlas is fully see-through or fully opaque.
bool AtlasAlphaIsBinary(const QuadBlit::Source& src)
{
    if (!src.hasAlpha)
    {
        return true;  // opaque, or see-through only by its colour key
    }
    if (!src.pixels || src.bytesPerPixel <= src.alphaOffset)
    {
        return false;
    }
    const int32_t stride = src.stride > 0 ? src.stride : src.width * src.bytesPerPixel;
    for (int32_t y = 0; y < src.height; ++y)
    {
        const uint8_t* p = src.pixels + static_cast<size_t>(y) * stride + src.alphaOffset;
        for (int32_t x = 0; x < src.width; ++x, p += src.bytesPerPixel)
        {
            if (*p != 0 && *p != 255)
            {
                return false;
            }
        }
    }
    return true;
}
}  // namespace

TilemapRenderPass::FramePlan* TilemapRenderPass::FindFramePlan(const Deki::Object* obj)
{
    for (FramePlan& p : m_FramePlans)
    {
        if (p.obj == obj)
        {
            return &p;
        }
    }
    return nullptr;
}

TilemapRenderPass::FramePlan& TilemapRenderPass::StartFramePlan(const Deki::Object* obj, const TilemapComponent* tc,
                                                                const DekiRendering::RenderContext& ctx)
{
    FramePlan* plan = FindFramePlan(obj);
    if (!plan)
    {
        // One for each map object drawn; an object no longer drawn gives its
        // place up.
        for (FramePlan& p : m_FramePlans)
        {
            if (p.frame != ctx.contentFrame)
            {
                plan = &p;
                break;
            }
        }
        if (!plan)
        {
            m_FramePlans.emplace_back();
            plan = &m_FramePlans.back();
        }
    }
    plan->obj = obj;
    plan->tc = tc;
    plan->frame = ctx.contentFrame;
    plan->buffer = ctx.buffer;
    plan->width = ctx.width;
    plan->height = ctx.height;
    plan->viewWidth = ctx.viewWidth;
    plan->viewHeight = ctx.viewHeight;
    plan->fromCopies = true;
    plan->groups.clear();
    return *plan;
}

bool TilemapRenderPass::DrawPartFromCopies(Deki::Object* obj, const TilemapComponent* tc,
                                           DekiRendering::RenderContext& ctx)
{
    // Everything DrawFromCopy works out before it copies is the same in
    // every part of a frame but the part itself. A frame redrawn in parts
    // calls Execute once a part, a dozen times a frame with as many sprites
    // moving, so the frame's first part records what it found and the others
    // only make the checks that depend on the part: all of them, for every
    // group, before anything is drawn, so a part that cannot come from the
    // copies is drawn the long way from its start.
    const FramePlan* plan = FindFramePlan(obj);
    if (!plan || plan->frame != ctx.contentFrame || !plan->fromCopies || plan->tc != tc || plan->buffer != ctx.buffer ||
        plan->width != ctx.width || plan->height != ctx.height || plan->viewWidth != ctx.viewWidth ||
        plan->viewHeight != ctx.viewHeight || ctx.viewLeft != 0 || ctx.viewTop != 0 ||
        plan->groups.size() > kMaxViewCopies)
    {
        return false;
    }
    const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
    const Deki::Rect target{ std::max<int32_t>(0, clip.left), std::max<int32_t>(0, clip.top),
                             std::min<int32_t>(ctx.width, clip.right), std::min<int32_t>(ctx.height, clip.bottom) };
    const Deki::Rect cleared = ctx.redraw.Empty() ? Deki::Rect{ 0, 0, ctx.width, ctx.height } : ctx.redraw;
    uint8_t clearPixel[4] = {};
    // Whether the clear still waits when a group is reached: a group that
    // draws runs it or takes it.
    bool onClear = target.left == cleared.left && target.top == cleared.top && target.right == cleared.right &&
                   target.bottom == cleared.bottom && QuadBlit::PendingClearColour(ctx.buffer, clearPixel);
    ViewCopy* copies[kMaxViewCopies] = {};
    for (size_t i = 0; i < plan->groups.size(); ++i)
    {
        const FramePlan::Group& g = plan->groups[i];
        if (g.boxed &&
            (g.boxRight <= static_cast<float>(target.left) || g.boxLeft >= static_cast<float>(target.right) ||
             g.boxBottom <= static_cast<float>(target.top) || g.boxTop >= static_cast<float>(target.bottom)))
        {
            continue;  // away from all its tiles: nothing is drawn
        }
        ViewCopy* copy = nullptr;
        for (ViewCopy& c : m_ViewCopies)
        {
            if (c.tc == tc && c.layerMask == g.layerMask)
            {
                copy = &c;
                break;
            }
        }
        if (!copy || copy->preparedFrame != ctx.contentFrame || !copy->usable || (copy->opaque && !onClear))
        {
            return false;
        }
        if (!target.Empty())
        {
            const Deki::Rect from{ target.left + copy->offsetX, target.top + copy->offsetY,
                                   target.right + copy->offsetX, target.bottom + copy->offsetY };
            if (!Inside(from, copy->valid))
            {
                return false;
            }
            onClear = false;
        }
        copies[i] = copy;
    }
    for (size_t i = 0; i < plan->groups.size(); ++i)
    {
        if (copies[i])
        {
            CopyPart(*copies[i], ctx, target);
        }
    }
    return true;
}

bool TilemapRenderPass::CopyPart(ViewCopy& copy, DekiRendering::RenderContext& ctx, const Deki::Rect& target)
{
    // That part of the buffer, in the copy.
    if (target.Empty())
    {
        return true;
    }
    const int32_t toCopyX = copy.offsetX + ctx.viewLeft;
    const int32_t toCopyY = copy.offsetY + ctx.viewTop;
    const Deki::Rect from{ target.left + toCopyX, target.top + toCopyY, target.right + toCopyX,
                           target.bottom + toCopyY };
    if (!Inside(from, copy.valid))
    {
        return false;
    }
    Deki::Rect pieces[4];
    int32_t kx[4], ky[4];
    const int n = SplitWrapped(from, copy.width, copy.height, pieces, kx, ky);
    const int32_t bpp = copy.BytesPerPixel();
    const size_t rowBytes = static_cast<size_t>(copy.width) * bpp;
    if (copy.opaque)
    {
        QuadBlit::DropPendingClear();  // the copy holds it, under the map
    }
    for (int i = 0; i < n; ++i)
    {
        const Deki::Rect& piece = pieces[i];
        const int32_t x = piece.left - kx[i] * copy.width;
        const int32_t y = piece.top - ky[i] * copy.height;
        QuadBlit::Source source = QuadBlit::MakeSource(
            copy.pixels.Data() + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * bpp, piece.Width(),
            piece.Height(), copy.opaque ? QuadBlit::PixelLayout::RGB565() : QuadBlit::PixelLayout::RGB565A8());
        source.stride = static_cast<int32_t>(rowBytes);
        QuadBlit::BlitScaled(source, ctx.buffer, ctx.width, ctx.height, ctx.format, piece.left - toCopyX,
                             piece.top - toCopyY, piece.Width(), piece.Height(), 255, 255, 255, 255);
    }
    return true;
}

bool TilemapRenderPass::DrawFromCopy(Deki::Object* obj, DekiRendering::RenderContext& ctx, uint32_t layerMask,
                                     float shiftX, float shiftY, uint32_t layerTint, float layerOpacity,
                                     uint8_t blendMode, FramePlan::Group* group)
{
    if (group)
    {
        group->copied = false;
        group->boxed = false;
    }
    // Only where the copy's pixels are the ones drawing would give.
    auto* tc = obj ? obj->GetComponent<TilemapComponent>() : nullptr;
    Tilemap* tm = tc ? tc->tilemap.Get() : nullptr;
    // Frames drawn whole, or redrawn in parts: a strip of a frame draws as
    // before.
    const bool strip = (ctx.viewWidth > 0 && ctx.viewWidth != ctx.width) ||
                       (ctx.viewHeight > 0 && ctx.viewHeight != ctx.height) || ctx.viewLeft != 0 || ctx.viewTop != 0;
    if (!tm || !tm->Streamer() || !ctx.buffer || !ctx.camera || !ctx.cam.valid || blendMode != 0 || strip ||
        ctx.format != Deki::ColorFormat::RGB565 || ctx.buffer == QuadBlit::SinkTarget())
    {
        return false;
    }
    uint8_t tintR = 255, tintG = 255, tintB = 255, tintA = 255;
    CombineTint(tc->tintColor, layerTint, layerOpacity, tintR, tintG, tintB, tintA);
    const float tilePPM = (tc->pixelsPerMeter > 0.0f) ? tc->pixelsPerMeter : 1.0f;
    if (tintR != 255 || tintG != 255 || tintB != 255 || tintA != 255 || ctx.cam.ppm != tilePPM || ctx.cam.snapStep > 1)
    {
        return false;
    }
    // On whole pixels, as a camera that scrolls needs (AddChanges).
    auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1.0e-3f; };
    float anchorX = 0.0f, anchorY = 0.0f;
    tm->GetAnchor(anchorX, anchorY);
    if (!whole((obj->GetWorldX() + shiftX) * tilePPM) || !whole((obj->GetWorldY() + shiftY) * tilePPM) ||
        !whole(anchorX) || !whole(anchorY) || !whole(tm->BoundsLeft()) ||
        (tm->IsIsometric() && (tm->TileWidth() % 2 != 0 || tm->TileHeight() % 2 != 0)))
    {
        return false;
    }

    // The tilesets as DrawLayers finds them, each looked at once.
    if (auto* mgr = Deki::AssetManager::Get())
    {
        if (mgr->GetEpoch() != m_CachesEpoch)
        {
            return false;  // DrawLayers starts the caches again first
        }
    }
    TilesetCache& cache = GetCache(tm);
    RefreshCache(tm, cache);
    if (cache.binaryAlpha.size() != cache.sources.size())
    {
        cache.binaryAlpha.assign(cache.sources.size(), 0);
    }
    uint32_t ready = 0;
    for (size_t i = 0; i < cache.sources.size(); ++i)
    {
        if (!cache.ready[i])
        {
            continue;
        }
        ++ready;
        if (cache.binaryAlpha[i] == 0)
        {
            cache.binaryAlpha[i] = AtlasAlphaIsBinary(cache.sources[i]) ? 1 : 2;
        }
        if (cache.binaryAlpha[i] != 1)
        {
            return false;
        }
    }

    // The part of the buffer this draw may write.
    const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
    const Deki::Rect target{ std::max<int32_t>(0, clip.left), std::max<int32_t>(0, clip.top),
                             std::min<int32_t>(ctx.width, clip.right), std::min<int32_t>(ctx.height, clip.bottom) };

    // A map that does not loop draws nothing outside the box its tiles can
    // reach: there, nothing is drawn (and nothing kept), as DrawLayers would.
    if (!tc->loopX && !tc->loopY)
    {
        if (!cache.boxKnown || cache.boxReady != ready)
        {
            cache.boxKnown = true;
            cache.boxReady = ready;
            const auto& index = tm->Index();
            if (index.empty())
            {
                cache.boxLeft = cache.boxTop = cache.boxRight = cache.boxBottom = 0.0f;
            }
            else
            {
                int32_t cx0 = index[0].chunkX, cx1 = cx0, cy0 = index[0].chunkY, cy1 = cy0;
                for (const ChunkIndexEntry& e : index)
                {
                    cx0 = std::min(cx0, e.chunkX);
                    cx1 = std::max(cx1, e.chunkX);
                    cy0 = std::min(cy0, e.chunkY);
                    cy1 = std::max(cy1, e.chunkY);
                }
                const float cw = static_cast<float>(tm->ChunkWidth()), ch = static_cast<float>(tm->ChunkHeight());
                const float columns[2] = { cx0 * cw, (cx1 + 1) * cw };
                const float rows[2] = { cy0 * ch, (cy1 + 1) * ch };
                bool first = true;
                for (float column : columns)
                {
                    for (float row : rows)
                    {
                        float x = 0.0f, y = 0.0f;
                        tm->TileToPixels(column, row, x, y);
                        cache.boxLeft = first ? x : std::min(cache.boxLeft, x);
                        cache.boxRight = first ? x : std::max(cache.boxRight, x);
                        cache.boxTop = first ? y : std::min(cache.boxTop, y);
                        cache.boxBottom = first ? y : std::max(cache.boxBottom, y);
                        first = false;
                    }
                }
                // Grown by the largest tile image and its offset each way.
                float pad = static_cast<float>(tm->TileWidth() + tm->TileHeight()) + 4.0f;
                for (size_t i = 0; i < cache.tilesets.size(); ++i)
                {
                    const Tileset* ts = cache.tilesets[i];
                    if (!ts || !cache.ready[i])
                    {
                        continue;
                    }
                    float left = 0.0f, top = 0.0f, w = 0.0f, h = 0.0f;
                    ts->DrawnTileRect(tm->TileWidth(), tm->TileHeight(), left, top, w, h);
                    const float offX = std::fabs(static_cast<float>(ts->TileOffsetX())) *
                                       (w / std::max(1, static_cast<int>(ts->TileWidth())));
                    const float offY = std::fabs(static_cast<float>(ts->TileOffsetY())) *
                                       (h / std::max(1, static_cast<int>(ts->TileHeight())));
                    pad = std::max(pad, w + h + std::fabs(left) + std::fabs(top) + offX + offY +
                                            static_cast<float>(tm->TileWidth() + tm->TileHeight()) + 4.0f);
                }
                cache.boxLeft -= pad;
                cache.boxTop -= pad;
                cache.boxRight += pad;
                cache.boxBottom += pad;
            }
        }
        // That box on screen, through this camera.
        const float invTilePPM = 1.0f / tilePPM;
        const float originX = obj->GetWorldX() + shiftX - anchorX * invTilePPM;
        const float originY = obj->GetWorldY() + shiftY + anchorY * invTilePPM;
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        ctx.cam.WorldToScreen(originX + cache.boxLeft * invTilePPM, originY - cache.boxTop * invTilePPM, left, top);
        ctx.cam.WorldToScreen(originX + cache.boxRight * invTilePPM, originY - cache.boxBottom * invTilePPM, right,
                              bottom);
        if (group)
        {
            group->boxed = true;
            group->boxLeft = left;
            group->boxTop = top;
            group->boxRight = right;
            group->boxBottom = bottom;
        }
        if (cache.boxRight <= cache.boxLeft || right <= static_cast<float>(target.left) ||
            left >= static_cast<float>(target.right) || bottom <= static_cast<float>(target.top) ||
            top >= static_cast<float>(target.bottom))
        {
            return true;
        }
    }

    // The whole view, whatever part of it this draw is.
    const int32_t viewW = ctx.viewWidth > 0 ? ctx.viewWidth : ctx.width;
    const int32_t viewH = ctx.viewHeight > 0 ? ctx.viewHeight : ctx.height;
    DekiRendering::FrameCamera view = ctx.cam;
    view.halfW += static_cast<float>(ctx.viewLeft);
    view.halfH += static_cast<float>(ctx.viewTop);

    const Deki::AssetManager* assets = Deki::AssetManager::Get();
    const uint64_t epoch = assets ? assets->GetEpoch() : 0;
    DekiRendering::Fingerprint f;
    f.Add(static_cast<const void*>(tm));
    f.Add(obj->GetWorldX());
    f.Add(obj->GetWorldY());
    f.Add(layerMask);
    f.Add(shiftX);
    f.Add(shiftY);
    f.Add(static_cast<uint32_t>(tc->visibleLayerMask));
    f.Add(tm->HiddenLayerMask());
    f.Add(tc->pixelsPerMeter);
    f.Add(static_cast<uint32_t>(tc->loopX) | (static_cast<uint32_t>(tc->loopY) << 1) |
          (static_cast<uint32_t>(tc->pixelSnap) << 2));
    f.Add(static_cast<uint32_t>(tc->wrapPeriodX));
    f.Add(static_cast<uint32_t>(tc->wrapPeriodY));
    f.Add(ready);
    f.Add(static_cast<uint32_t>(epoch));
    f.Add(static_cast<uint32_t>(epoch >> 32));
    f.Add(view.ppm);
    f.Add(static_cast<uint32_t>(view.snapStep));
    f.Add(static_cast<uint32_t>(viewW));
    f.Add(static_cast<uint32_t>(viewH));
    const uint64_t signature = f.Value();

    // Whether the map is the first thing drawn there: all of it waits for
    // the clear.
    const Deki::Rect cleared = ctx.redraw.Empty() ? Deki::Rect{ 0, 0, ctx.width, ctx.height } : ctx.redraw;
    uint8_t clearPixel[4] = {};
    const bool onClear = target.left == cleared.left && target.top == cleared.top && target.right == cleared.right &&
                         target.bottom == cleared.bottom && QuadBlit::PendingClearColour(ctx.buffer, clearPixel);

    ViewCopy* copy = nullptr;
    for (ViewCopy& c : m_ViewCopies)
    {
        if (c.tc == tc && c.layerMask == layerMask)
        {
            copy = &c;
            break;
        }
    }
    if (copy && copy->opaque && !onClear)
    {
        return false;  // something is under the map here: drawn as before
    }
    if (!copy)
    {
        if (m_ViewCopies.size() >= kMaxViewCopies)
        {
            auto oldest = std::min_element(m_ViewCopies.begin(), m_ViewCopies.end(),
                                           [](const ViewCopy& a, const ViewCopy& b) { return a.lastUse < b.lastUse; });
            m_ViewCopies.erase(oldest);
        }
        m_ViewCopies.emplace_back();
        copy = &m_ViewCopies.back();
        copy->tc = tc;
        copy->layerMask = layerMask;
    }

    // Once a frame (every call when the frame has no id): brought up to date
    // for the whole view.
    if (ctx.contentFrame == 0 || copy->preparedFrame != ctx.contentFrame)
    {
        copy->preparedFrame = ctx.contentFrame;
        copy->usable = false;
        copy->lastUse = ++m_CopyClock;
        const int32_t width = viewW + 2 * kCopyMargin;
        const int32_t height = viewH + 2 * kCopyMargin;
        int32_t offsetX = 0, offsetY = 0;
        const bool sameClear = !copy->opaque || std::memcmp(copy->clear, clearPixel, 2) == 0;
        const bool placed = copy->signature == signature && copy->width == width && copy->height == height &&
                            sameClear && WholeInt(OffsetAcross(view) - OffsetAcross(copy->anchor), offsetX) &&
                            WholeInt(OffsetDown(copy->anchor) - OffsetDown(view), offsetY);
        if (!placed && !(OnWholePixels(OffsetAcross(view)) && OnWholePixels(OffsetDown(view))))
        {
            return false;  // a camera between pixels: a copy starts only on whole ones
        }
        if (!placed)
        {
            // Anything else moved, or the camera left whole pixels, or went
            // far: drawn again from this camera.
            const bool changed = copy->signature != signature || copy->width != width || copy->height != height;
            if (changed)
            {
                copy->off = false;
                copy->opaque = onClear;  // a new copy: over the clear wherever it can be
            }
            if (copy->opaque)
            {
                std::memcpy(copy->clear, clearPixel, sizeof(copy->clear));
            }
            copy->signature = signature;
            copy->anchor = view;
            copy->valid = Deki::Rect{};
            copy->band = Deki::Rect{};
            offsetX = offsetY = 0;
            const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * copy->BytesPerPixel();
            if (copy->pixels.Count() != bytes)
            {
                copy->pixels = Deki::Buffer<uint8_t>();
            }
            copy->width = width;
            copy->height = height;
            if (!copy->off && !copy->pixels.Data() && !copy->pixels.Allocate(bytes, Deki::Memory::External))
            {
                copy->off = true;  // no memory for it: drawn as before
            }
        }
        if (copy->off || !WholeInt(OffsetAcross(view) - OffsetAcross(copy->anchor), offsetX) ||
            !WholeInt(OffsetDown(copy->anchor) - OffsetDown(view), offsetY))
        {
            return false;
        }

        // What it must hold now: the view. The copy holds a rect of the map
        // around it, and grows a band at a time ahead of where it moves.
        const Deki::Rect need{ offsetX, offsetY, offsetX + viewW, offsetY + viewH };
        Deki::Rect& valid = copy->valid;
        const Deki::Rect overlap{ std::max(valid.left, need.left), std::max(valid.top, need.top),
                                  std::min(valid.right, need.right), std::min(valid.bottom, need.bottom) };
        if (valid.Empty() || overlap.Empty())
        {
            // Drawn afresh: the view now, its margin as it moves.
            copy->band = Deki::Rect{};
            valid = Deki::Rect{};
            if (DrawIntoCopy(*copy, obj, ctx, need, layerMask, shiftX, shiftY, layerTint, layerOpacity))
            {
                valid = need;
            }
        }
        else
        {
            if (copy->band.Empty())
            {
                // The side the view is closest to, if it is within the look
                // ahead: a band out to the margin past the view there. The
                // other side gives way, as far as the view lets it.
                const int32_t gaps[4] = { valid.right - need.right, need.left - valid.left, valid.bottom - need.bottom,
                                          need.top - valid.top };
                int side = -1;
                for (int k = 0; k < 4; ++k)
                {
                    if (gaps[k] < kCopyLookAhead && (side < 0 || gaps[k] < gaps[side]))
                    {
                        side = k;
                    }
                }
                if (side >= 0)
                {
                    const bool across = side < 2;
                    const int32_t size = across ? width : height;
                    const int32_t span = across ? valid.Width() : valid.Height();
                    // How far the far side may give way without cutting the view.
                    const int32_t room = side == 0   ? need.left - valid.left
                                         : side == 1 ? valid.right - need.right
                                         : side == 2 ? need.top - valid.top
                                                     : valid.bottom - need.bottom;
                    const int32_t grow =
                        std::min<int32_t>(kCopyMargin - gaps[side], size - span + std::max<int32_t>(0, room));
                    const int32_t trim = std::max<int32_t>(0, span + grow - size);
                    if (grow > 0)
                    {
                        switch (side)
                        {
                            case 0:
                                valid.left += trim;
                                copy->band = Deki::Rect{ valid.right, valid.top, valid.right + grow, valid.bottom };
                                break;
                            case 1:
                                valid.right -= trim;
                                copy->band = Deki::Rect{ valid.left - grow, valid.top, valid.left, valid.bottom };
                                break;
                            case 2:
                                valid.top += trim;
                                copy->band = Deki::Rect{ valid.left, valid.bottom, valid.right, valid.bottom + grow };
                                break;
                            default:
                                valid.bottom -= trim;
                                copy->band = Deki::Rect{ valid.left, valid.top - grow, valid.right, valid.top };
                                break;
                        }
                        copy->bandPieces = 0;
                    }
                }
            }
            if (!copy->band.Empty())
            {
                // Its next piece; once all are drawn the band is part of the copy.
                Deki::Rect pieces[4];
                int32_t kx[4], ky[4];
                const int n = SplitWrapped(copy->band, width, height, pieces, kx, ky);
                if (copy->bandPieces < n && DrawIntoCopy(*copy, obj, ctx, pieces[copy->bandPieces], layerMask, shiftX,
                                                         shiftY, layerTint, layerOpacity))
                {
                    ++copy->bandPieces;
                }
                if (copy->bandPieces >= n)
                {
                    valid = Deki::Rect{ std::min(valid.left, copy->band.left), std::min(valid.top, copy->band.top),
                                        std::max(valid.right, copy->band.right),
                                        std::max(valid.bottom, copy->band.bottom) };
                    copy->band = Deki::Rect{};
                }
            }
        }
        if (copy->off)
        {
            return false;  // an animated tile: drawn as before
        }
        copy->offsetX = offsetX;
        copy->offsetY = offsetY;
        copy->usable = !copy->valid.Empty();
    }
    if (!copy->usable)
    {
        return false;
    }

    if (!CopyPart(*copy, ctx, target))
    {
        return false;
    }
    if (group)
    {
        group->copied = true;
    }
    return true;
}

bool TilemapRenderPass::DrawIntoCopy(ViewCopy& copy, Deki::Object* obj, const DekiRendering::RenderContext& ctx,
                                     const Deki::Rect& rect, uint32_t layerMask, float shiftX, float shiftY,
                                     uint32_t layerTint, float layerOpacity)
{
    Deki::Rect pieces[4];
    int32_t kx[4], ky[4];
    const int n = SplitWrapped(rect, copy.width, copy.height, pieces, kx, ky);
    const uint32_t missed = m_MissedChunks;
    const uint32_t animated = s_AnimatedDraws;
    const int32_t bpp = copy.BytesPerPixel();
    const size_t rowBytes = static_cast<size_t>(copy.width) * bpp;

    // The frame's clips are set aside: the copy has its own.
    QuadBlit::SwapClipState(m_ClipSwap);
    for (int i = 0; i < n; ++i)
    {
        const Deki::Rect at{ pieces[i].left - kx[i] * copy.width, pieces[i].top - ky[i] * copy.height,
                             pieces[i].right - kx[i] * copy.width, pieces[i].bottom - ky[i] * copy.height };
        // Cleared as the frame is: to its clear colour, or to see-through.
        for (int32_t y = at.top; y < at.bottom; ++y)
        {
            uint8_t* row = copy.pixels.Data() + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(at.left) * bpp;
            if (copy.opaque)
            {
                for (int32_t x = 0; x < at.Width(); ++x)
                {
                    row[x * 2] = copy.clear[0];
                    row[x * 2 + 1] = copy.clear[1];
                }
            }
            else
            {
                std::memset(row, 0, static_cast<size_t>(at.Width()) * 3);
            }
        }
        DekiRendering::RenderContext into = ctx;
        into.buffer = copy.pixels.Data();
        into.width = copy.width;
        into.height = copy.height;
        into.format = copy.opaque ? Deki::ColorFormat::RGB565 : Deki::ColorFormat::RGB565A8;
        into.viewLeft = into.viewTop = 0;
        into.viewWidth = copy.width;
        into.viewHeight = copy.height;
        into.redraw = Deki::Rect{};
        into.clear = nullptr;
        into.contentFrame = 0;
        into.scrollX = into.scrollY = 0;
        into.cam = copy.anchor;
        into.cam.halfW -= static_cast<float>(kx[i] * copy.width);
        into.cam.halfH -= static_cast<float>(ky[i] * copy.height);
        m_CopyCentre = true;
        m_CopyCentreX = into.cam.camX + (static_cast<float>(copy.width) * 0.5f - into.cam.halfW) / into.cam.ppm;
        m_CopyCentreY = into.cam.camY + (into.cam.halfH - static_cast<float>(copy.height) * 0.5f) / into.cam.ppm;
        QuadBlit::PushClipRect(at.left, at.top, at.right, at.bottom);
        DrawLayers(obj, into, layerMask, shiftX, shiftY, layerTint, layerOpacity, 0);
        QuadBlit::PopClipRect();
        m_CopyCentre = false;
    }
    QuadBlit::SwapClipState(m_ClipSwap);

    if (s_AnimatedDraws != animated)
    {
        copy.off = true;  // its pixels change with time: drawn as before
        copy.valid = Deki::Rect{};
        return false;
    }
    return m_MissedChunks == missed;  // else drawn again once the chunks are in
}

}  // namespace DekiTiledMap

// Registers with autoAttach=true, so DekiRenderingInit attaches the pass to
// the active DekiRendering::Standard2DRenderer whenever this package is
// loaded. The project's .rpipeline need not mention "tilemap", but can, to
// order it against other passes (clip2d, say).
void DekiTiledMap::RegisterTilemapRenderPass()
{
    DekiRendering::RenderPassInfo info;
    info.factory = []() -> DekiRendering::RenderPass* { return new DekiTiledMap::TilemapRenderPass(); };
    info.autoAttach = true;
    DekiRendering::DekiRenderPassRegistry::Register(DekiTiledMap::TilemapRenderPass::kRegistryName, info);
}

// In a DLL this also registers the pass, and takes it out again on unload. A
// firmware links the package from a static library and drops this object, so
// DekiTilemapInitSystem registers the pass there.
namespace
{
struct TilemapRenderPassRegistrar
{
    TilemapRenderPassRegistrar() { DekiTiledMap::RegisterTilemapRenderPass(); }
    // Unregister on DLL unload, so the factory (code in this package) does
    // not outlive the DLL and crash deki-rendering's registry teardown.
    ~TilemapRenderPassRegistrar()
    {
        DekiRendering::DekiRenderPassRegistry::Unregister(DekiTiledMap::TilemapRenderPass::kRegistryName);
    }
};
static TilemapRenderPassRegistrar s_TilemapPassRegistrar;
}  // namespace
