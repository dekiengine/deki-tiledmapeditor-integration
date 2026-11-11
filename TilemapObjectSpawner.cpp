#include "TilemapObjectSpawner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <deki/ICamera.h>
#include <deki/LogSystem.h>
#include <deki/Math.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/ScreenAnchor.h>
#include <deki/Time.h>
#include <deki/assets/AssetManager.h>

#include "SpriteComponent.h"
#include "TilemapComponent.h"
#include "TileChunk.h"
#include "Tileset.h"

namespace DekiTiledMap
{

namespace
{

// Each object layer's objects get their own band of sorting orders, and the
// tile layers above it the middle of the band after: what Tiled stacks above
// an object layer draws above its objects. A band holds 2^19 orders above
// and below its middle, 32768 meters at 16 orders per meter.
constexpr int32_t kLayerBand = 1 << 20;

// The object's "scene_guid" string property, or nullptr.
const DekiTiledMap::DTilemapProperty* FindSceneGuidProperty(const DekiTiledMap::Tilemap& tm,
                                                            const DekiTiledMap::DTilemapObject& obj)
{
    const DekiTiledMap::DTilemapProperty* p = tm.FindProperty(obj, "scene_guid");
    return p && p->type == static_cast<uint32_t>(DekiTiledMap::DPropertyType::String) ? p : nullptr;
}

// Adds the parallax placer once a spawner exists in this code, and takes it
// out when the code is unloaded, so the engine never calls into an unloaded
// package.
struct ParallaxPlacer
{
    ParallaxPlacer() { Deki::ScreenAnchor::AddPlacer(&TilemapObjectSpawner::PlaceParallaxLayers); }
    ~ParallaxPlacer() { Deki::ScreenAnchor::RemovePlacer(&TilemapObjectSpawner::PlaceParallaxLayers); }
};

}  // namespace

TilemapObjectSpawner* TilemapObjectSpawner::s_First = nullptr;

TilemapObjectSpawner::TilemapObjectSpawner()
{
    static ParallaxPlacer s_Placer;
    m_Next = s_First;
    if (s_First)
    {
        s_First->m_Prev = this;
    }
    s_First = this;
}

TilemapObjectSpawner::~TilemapObjectSpawner()
{
    if (m_Prev)
    {
        m_Prev->m_Next = m_Next;
    }
    else
    {
        s_First = m_Next;
    }
    if (m_Next)
    {
        m_Next->m_Prev = m_Prev;
    }
}

void TilemapObjectSpawner::PlaceParallaxLayers(const Deki::Scene* scene, const Deki::ICamera& camera, int screenWidth,
                                               int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0)
    {
        return;
    }
    float viewWorldX = 0.0f, viewWorldY = 0.0f;
    camera.ScreenToWorld(static_cast<float>(screenWidth) * 0.5f, static_cast<float>(screenHeight) * 0.5f, screenWidth,
                         screenHeight, viewWorldX, viewWorldY);
    for (TilemapObjectSpawner* s = s_First; s; s = s->m_Next)
    {
        Deki::Object* owner = s->GetOwner();
        Tilemap* tm = s->tilemap.Get();
        if (s->m_Parallax.empty() || !owner || !tm || (owner->GetOwnerScene() != scene && !owner->IsPersistent()))
        {
            continue;
        }
        // The view's centre in Tiled pixels, as the map's render pass works it
        // out: a layer moves by (1 - factor) times it plus the Parallax Origin.
        const float ppm = (s->pixelsPerMeter > 0.0f) ? s->pixelsPerMeter : 1.0f;
        float anchorX = 0.0f, anchorY = 0.0f;
        tm->GetAnchor(anchorX, anchorY);
        const float viewX = (viewWorldX - owner->GetWorldX()) * ppm + anchorX + tm->BoundsLeft();
        const float viewY = anchorY - (viewWorldY - owner->GetWorldY()) * ppm;
        for (ParallaxLayer& layer : s->m_Parallax)
        {
            Deki::Object* holder = layer.objectId != 0 ? s->FindSpawned(layer.objectId) : nullptr;
            if (!holder)
            {
                layer.objectId = 0;  // destroyed
                continue;
            }
            const float x = (1.0f - layer.factorX) * (viewX + tm->ParallaxOriginX()) / ppm;
            const float y = -(1.0f - layer.factorY) * (viewY + tm->ParallaxOriginY()) / ppm;
            if (holder->GetX() != x || holder->GetY() != y)
            {
                holder->SetX(x);
                holder->SetY(y);
            }
        }
    }
}

void TilemapObjectSpawner::Awake()
{
    DekiTiledMap::Tilemap* tm = tilemap.Get();
    if (!tm)
    {
        DEKI_LOG_WARNING("TilemapObjectSpawner: no tilemap assigned");
        return;
    }

    Deki::Object* owner = GetOwner();
    if (!owner)
    {
        return;
    }
    Deki::Scene* targetScene = owner->GetOwnerScene();
    if (!targetScene)
    {
        DEKI_LOG_ERROR("TilemapObjectSpawner: spawner's owner has no scene");
        return;
    }

    const float ppm = (pixelsPerMeter > 0.0f) ? pixelsPerMeter : 1.0f;
    const auto& objs = tm->Objects();
    auto* mgr = Deki::AssetManager::Get();

    // Spawned objects go in a holder beside the owner, not under it: the
    // owner usually draws the map, and what is under a drawn object draws
    // with it as one. In a holder, which draws nothing, they sort with the
    // rest of the scene, so a character can walk behind them.
    Deki::Object* holder = nullptr;
    if (Deki::Object* parent = owner->GetParent())
    {
        holder = new Deki::Object();
        holder->SetName(owner->GetName() + " objects");
        parent->AddChild(holder);
    }
    else
    {
        holder = targetScene->CreateDekiObject(owner->GetName() + " objects");
    }
    holder->SetX(owner->GetX());
    holder->SetY(owner->GetY());
    holder->SetRotation(owner->GetLocalRotation());
    holder->SetScale(owner->GetScaleX(), owner->GetScaleY());
    m_MSpawned.push_back(holder);
    m_HolderId = holder->GetId();

    // A layer with parallax gets an object of its own in the holder, moved
    // with the view before each frame (PlaceParallaxLayers).
    std::vector<Deki::Object*> layerHolders(tm->ObjectLayers().size(), holder);
    for (size_t i = 0; i < layerHolders.size(); ++i)
    {
        const DLayerLook* look = tm->FindLayerLook(DLayerKind::Object, static_cast<uint32_t>(i));
        if (!look || (look->parallaxX == 1.0f && look->parallaxY == 1.0f))
        {
            continue;
        }
        auto* layerHolder = new Deki::Object();
        layerHolder->SetName(tm->ObjectLayers()[i].name);
        holder->AddChild(layerHolder);
        m_MSpawned.push_back(layerHolder);
        layerHolders[i] = layerHolder;
        m_Parallax.push_back({ layerHolder->GetId(), look->parallaxX, look->parallaxY });
    }

    for (const auto& obj : objs)
    {
        // Its object layer's offset, tint and opacity, as Tiled draws them.
        const uint32_t layerIndex = tm->ObjectLayerOf(obj);
        const DLayerLook* look = tm->FindLayerLook(DLayerKind::Object, layerIndex);
        Deki::Object* into = layerIndex < layerHolders.size() ? layerHolders[layerIndex] : holder;
        const float layerX = look ? look->offsetX : 0.0f;
        const float layerY = look ? look->offsetY : 0.0f;

        // Where the map draws that point, relative to the owner.
        float px = 0.0f, py = 0.0f, anchorX = 0.0f, anchorY = 0.0f;
        tm->ObjectToPixels(static_cast<float>(obj.x), static_cast<float>(obj.y), px, py);
        px += layerX;
        py += layerY;
        tm->GetAnchor(anchorX, anchorY);
        const float wx = (px - anchorX) / ppm;
        const float wy = (anchorY - py) / ppm;

        const DekiTiledMap::DTilemapProperty* guidProp = FindSceneGuidProperty(*tm, obj);
        if (guidProp)
        {
            std::string guid = tm->GetString(guidProp->valueOffset);
            if (guid.empty())
            {
                DEKI_LOG_ERROR("TilemapObjectSpawner: object %u has empty scene_guid", obj.id);
                continue;
            }
            Deki::Scene* scene =
                mgr ? static_cast<Deki::Scene*>(mgr->LoadByGuidAndType(guid, Deki::Scene::kAssetTypeName)) : nullptr;
            if (!scene)
            {
                DEKI_LOG_ERROR("TilemapObjectSpawner: scene '%s' not found for object %u", guid.c_str(), obj.id);
                continue;
            }
            Deki::Object* spawned = scene->Instantiate(targetScene, wx, wy);
            if (!spawned)
            {
                continue;
            }
            // Tiled stores clockwise degrees; the engine uses counter-clockwise radians.
            spawned->SetRotation(-obj.rotation * Deki::Math::kDegToRad);
            into->AddChild(spawned);
            m_MSpawned.push_back(spawned);
            continue;
        }

        if (obj.gid != 0)
        {
            uint32_t localId = 0;
            const DekiTiledMap::TilesetRef* ref = tm->ResolveTileset(obj.gid, localId);
            auto* tileset = ref && mgr ? static_cast<DekiTiledMap::Tileset*>(
                                             mgr->LoadByGuidAndType(ref->guid, DekiTiledMap::Tileset::kAssetTypeName))
                                       : nullptr;
            Deki2D::Sprite* image = tileset ? tileset->Atlas() : nullptr;
            if (!image)
            {
                DEKI_LOG_ERROR("TilemapObjectSpawner: object %u has a tile whose tileset or image is missing", obj.id);
                continue;
            }

            int sx = 0, sy = 0, sw = 0, sh = 0;
            tileset->GetTileRect(localId, sx, sy, sw, sh);
            const float drawnW = obj.width > 0 ? static_cast<float>(obj.width) : static_cast<float>(sw);
            const float drawnH = obj.height > 0 ? static_cast<float>(obj.height) : static_cast<float>(sh);

            float centreX = 0.0f, centreY = 0.0f;
            tm->TileObjectCentre(obj, drawnW, drawnH, tileset->ObjectAlignment(), centreX, centreY);
            centreX += layerX;
            centreY += layerY;
            const float cx = (centreX + static_cast<float>(tileset->TileOffsetX()) - anchorX) / ppm;
            const float cy = (anchorY - centreY - static_cast<float>(tileset->TileOffsetY())) / ppm;

            Deki::Object* piece = new Deki::Object();
            piece->SetX(cx);
            piece->SetY(cy);
            piece->SetRotation(-obj.rotation * Deki::Math::kDegToRad);
            piece->SetScale(drawnW / static_cast<float>(sw), drawnH / static_cast<float>(sh));
            piece->SetName(obj.name[0] ? obj.name : "Tile object");
            into->AddChild(piece);

            auto* sprite = piece->AddComponent<Deki2D::SpriteComponent>();
            sprite->sprite = image;
            sprite->SetFrameRect(sx, sy, sw, sh);
            sprite->flipHorizontal = DekiTiledMap::GidFlipH(obj.gid);
            sprite->flipVertical = DekiTiledMap::GidFlipV(obj.gid);
            if (look)
            {
                auto times = [](uint32_t x, uint32_t y) { return static_cast<uint8_t>((x * y + 127u) / 255u); };
                const uint32_t opacity =
                    static_cast<uint32_t>(std::lround(std::clamp(look->opacity, 0.0f, 1.0f) * 255.0f));
                sprite->tintColor = Deki::Color(times(255u, look->tint & 0xFFu), times(255u, (look->tint >> 8) & 0xFFu),
                                                times(255u, (look->tint >> 16) & 0xFFu),
                                                times(times(255u, (look->tint >> 24) & 0xFFu), opacity));
            }

            // A tile animated in its tileset plays here too.
            const DTileAnimation* anim = tileset->GetAnimation(localId);
            const DTileAnimationFrame* frames = anim ? tileset->GetAnimationFrames(*anim) : nullptr;
            if (frames && anim->frameCount > 0)
            {
                AnimatedPiece animated;
                animated.objectId = piece->GetId();
                for (uint32_t i = 0; i < anim->frameCount; ++i)
                {
                    int fx = 0, fy = 0, fw = 0, fh = 0;
                    tileset->GetTileRect(frames[i].localId, fx, fy, fw, fh);
                    animated.rects.insert(animated.rects.end(), { fx, fy, fw, fh });
                    animated.totalMs += frames[i].durationMs;
                    animated.endsMs.push_back(animated.totalMs);
                }
                if (animated.totalMs > 0)
                {
                    m_AnimatedById[animated.objectId] = static_cast<uint32_t>(m_Animated.size());
                    m_Animated.push_back(std::move(animated));
                }
            }
            // Sorted by its point, so a Tile Offset moves the picture without
            // changing what it draws in front of.
            const float standing = owner->GetWorldY() + wy;
            // Floored, so every step of 1 / Sort Per Meter is its own order,
            // around 0 too; equal orders keep the layer's order, as in Tiled.
            sprite->sortingOrder = sortBase - static_cast<int>(std::floor(standing * static_cast<float>(sortPerMeter))) +
                                   kLayerBand * BandOf(*tm, obj);
            m_MSpawned.push_back(piece);
            continue;
        }

        // No scene_guid and no tile: an empty Deki::Object with the transform.
        Deki::Object* spawned = new Deki::Object();
        spawned->SetX(wx);
        spawned->SetY(wy);
        // Tiled stores clockwise degrees; the engine uses counter-clockwise radians.
        spawned->SetRotation(-obj.rotation * Deki::Math::kDegToRad);
        if (obj.name[0])
        {
            spawned->SetName(obj.name);
        }
        into->AddChild(spawned);
        m_MSpawned.push_back(spawned);
    }

    StackLayersAboveObjects(*tm, owner, holder);
}

int32_t TilemapObjectSpawner::BandOf(const Tilemap& tm, const DTilemapObject& obj) const
{
    // How many object layers come before this object's own.
    const uint32_t layer = tm.ObjectLayerOf(obj);
    return static_cast<int32_t>(layer);
}

void TilemapObjectSpawner::StackLayersAboveObjects(Tilemap& tm, Deki::Object* owner, Deki::Object* holder)
{
    const std::vector<DLayerLook>& looks = tm.Layers();
    auto* main = owner->GetComponent<TilemapComponent>();
    if (!main || main->tilemap.Get() != &tm || looks.empty())
    {
        return;
    }
    std::vector<size_t> objectLayersAt;
    for (size_t i = 0; i < looks.size(); ++i)
    {
        if (looks[i].kind == static_cast<uint8_t>(DLayerKind::Object))
        {
            objectLayersAt.push_back(i);
        }
    }
    if (objectLayersAt.empty())
    {
        return;
    }

    // The map's own component draws what is under the first object layer;
    // each run of tile and image layers above an object layer gets a
    // component of its own, sorted above that layer's objects.
    main->endLayer = static_cast<uint16_t>(objectLayersAt[0]);
    for (size_t k = 0; k < objectLayersAt.size(); ++k)
    {
        const size_t from = objectLayersAt[k] + 1;
        const size_t to = k + 1 < objectLayersAt.size() ? objectLayersAt[k + 1] : looks.size();
        if (from >= to)
        {
            continue;
        }
        Deki::Object* upper = new Deki::Object();
        const uint32_t objectLayer = looks[objectLayersAt[k]].index;
        const char* name = objectLayer < tm.ObjectLayers().size() ? tm.ObjectLayers()[objectLayer].name : "";
        upper->SetName(std::string("Layers above ") + name);
        holder->AddChild(upper);
        m_MSpawned.push_back(upper);

        auto* layers = upper->AddComponent<TilemapComponent>();
        layers->tilemap = main->tilemap;
        layers->visibleLayerMask = main->visibleLayerMask;
        layers->chunkPadding = main->chunkPadding;
        layers->chunkCacheKiB = main->chunkCacheKiB;
        layers->tintColor = main->tintColor;
        layers->pixelsPerMeter = main->pixelsPerMeter;
        layers->loopX = main->loopX;
        layers->wrapPeriodX = main->wrapPeriodX;
        layers->loopY = main->loopY;
        layers->wrapPeriodY = main->wrapPeriodY;
        layers->pixelSnap = main->pixelSnap;
        layers->firstLayer = static_cast<uint16_t>(from);
        layers->endLayer = static_cast<uint16_t>(to);
        layers->sortingOrder = sortBase + kLayerBand * static_cast<int32_t>(k) + kLayerBand / 2;
    }
}

Deki::Object* TilemapObjectSpawner::Holder() const
{
    Deki::Object* owner = GetOwner();
    if (!owner || m_HolderId == 0)
    {
        return nullptr;
    }
    Deki::Object* holder = nullptr;
    if (Deki::Object* parent = owner->GetParent())
    {
        for (Deki::Object* o : parent->GetChildren())
        {
            if (o && o->GetId() == m_HolderId)
            {
                holder = o;
                break;
            }
        }
    }
    else if (Deki::Scene* scene = owner->GetOwnerScene())
    {
        for (Deki::Object* o : scene->GetObjects())
        {
            if (o && o->GetId() == m_HolderId)
            {
                holder = o;
                break;
            }
        }
    }
    return holder && !holder->IsPendingDestroy() ? holder : nullptr;
}

Deki::Object* TilemapObjectSpawner::FindSpawned(uint32_t id) const
{
    Deki::Object* holder = id != 0 ? Holder() : nullptr;
    if (!holder)
    {
        return nullptr;
    }
    auto live = [](Deki::Object* o) { return o && !o->IsPendingDestroy(); };
    for (Deki::Object* child : holder->GetChildren())
    {
        if (!live(child))
        {
            continue;
        }
        if (child->GetId() == id)
        {
            return child;
        }
        for (Deki::Object* grandchild : child->GetChildren())
        {
            if (live(grandchild) && grandchild->GetId() == id)
            {
                return grandchild;
            }
        }
    }
    return nullptr;
}

void TilemapObjectSpawner::Update()
{
    // Every animated tile object keeps the same clock, as in Tiled.
    const uint32_t now = Deki::Time::GetTime();
    size_t due = 0;
    for (AnimatedPiece& piece : m_Animated)
    {
        piece.due = -1;
        if (piece.objectId == 0)
        {
            continue;
        }
        const uint32_t t = now % piece.totalMs;
        const int32_t frame =
            static_cast<int32_t>(std::upper_bound(piece.endsMs.begin(), piece.endsMs.end(), t) - piece.endsMs.begin());
        if (frame != piece.shown && frame < static_cast<int32_t>(piece.endsMs.size()))
        {
            piece.due = frame;
            ++due;
        }
    }
    if (due == 0)
    {
        return;
    }

    // One walk over the spawned objects for every piece whose frame changed.
    auto show = [this](Deki::Object* object)
    {
        if (!object || object->IsPendingDestroy())
        {
            return;
        }
        const auto found = m_AnimatedById.find(object->GetId());
        if (found == m_AnimatedById.end())
        {
            return;
        }
        AnimatedPiece& piece = m_Animated[found->second];
        auto* sprite = piece.due >= 0 ? object->GetComponent<Deki2D::SpriteComponent>() : nullptr;
        if (!sprite)
        {
            return;
        }
        const int32_t* r = &piece.rects[static_cast<size_t>(piece.due) * 4];
        sprite->SetFrameRect(r[0], r[1], r[2], r[3]);
        piece.shown = piece.due;
        piece.due = -1;
    };
    if (Deki::Object* holder = Holder())
    {
        for (Deki::Object* child : holder->GetChildren())
        {
            show(child);
            if (child && !child->IsPendingDestroy())
            {
                for (Deki::Object* grandchild : child->GetChildren())
                {
                    show(grandchild);
                }
            }
        }
    }
    // A piece still due was not found: destroyed, nothing left to animate.
    for (AnimatedPiece& piece : m_Animated)
    {
        if (piece.due >= 0)
        {
            piece.objectId = 0;
        }
    }
}

}  // namespace DekiTiledMap
