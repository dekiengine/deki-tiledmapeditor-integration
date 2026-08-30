#include "TilemapObjectSpawner.h"

#include <cstring>

#include "DekiLogSystem.h"
#include "DekiMath.h"
#include "DekiObject.h"
#include "Scene.h"
#include "assets/AssetManager.h"

namespace
{

// Walk the property pool entries that belong to a single object and look for
// the well-known "scene_guid" string property.
const DekiTilemap::DTilemapProperty* FindSceneGuidProperty(
    const DekiTilemap::Tilemap& tm,
    const DekiTilemap::DTilemapObject& obj)
{
    if (obj.propertyCount == 0) return nullptr;
    const auto& props = tm.Properties();
    if (props.empty()) return nullptr;
    // propertyOffset is treated as a property-array index baked by the editor.
    if (obj.propertyOffset + obj.propertyCount > props.size())
        return nullptr;

    for (uint32_t i = 0; i < obj.propertyCount; ++i)
    {
        const auto& p = props[obj.propertyOffset + i];
        if (p.type != static_cast<uint32_t>(DekiTilemap::DPropertyType::String))
            continue;
        std::string name = tm.GetString(p.nameOffset);
        if (name == "scene_guid")
            return &p;
    }
    return nullptr;
}

} // namespace

void TilemapObjectSpawner::Awake()
{
    DekiTilemap::Tilemap* tm = tilemap.Get();
    if (!tm)
    {
        DEKI_LOG_WARNING("TilemapObjectSpawner: no tilemap assigned");
        return;
    }

    DekiObject* owner = GetOwner();
    if (!owner) return;
    Scene* targetScene = owner->GetOwnerScene();
    if (!targetScene)
    {
        DEKI_LOG_ERROR("TilemapObjectSpawner: spawner's owner has no scene");
        return;
    }

    const float ppm = (pixelsPerMeter > 0.0f) ? pixelsPerMeter : 1.0f;
    const auto& objs = tm->Objects();
    auto* mgr = Deki::AssetManager::Get();

    for (const auto& obj : objs)
    {
        const float wx = static_cast<float>(obj.x) / ppm;
        const float wy = static_cast<float>(obj.y) / ppm;

        const DekiTilemap::DTilemapProperty* guidProp = FindSceneGuidProperty(*tm, obj);
        if (guidProp)
        {
            std::string guid = tm->GetString(guidProp->valueOffset);
            if (guid.empty())
            {
                DEKI_LOG_ERROR("TilemapObjectSpawner: object %u has empty scene_guid", obj.id);
                continue;
            }
            Scene* scene = mgr ? static_cast<Scene*>(
                mgr->LoadByGuidAndType(guid, Scene::AssetTypeName)) : nullptr;
            if (!scene)
            {
                DEKI_LOG_ERROR("TilemapObjectSpawner: scene '%s' not found for object %u",
                               guid.c_str(), obj.id);
                continue;
            }
            DekiObject* spawned = scene->Instantiate(targetScene, wx, wy);
            if (!spawned) continue;
            // Tiled stores rotation in degrees; engine convention is radians.
            spawned->SetRotation(obj.rotation * DekiMath::kDegToRad);
            owner->AddChild(spawned);
            m_spawned.push_back(spawned);
            continue;
        }

        // No scene_guid — bare DekiObject with transform only. SpriteComponent
        // synthesis for tile objects is a follow-up.
        DekiObject* spawned = new DekiObject();
        spawned->SetX(wx);
        spawned->SetY(wy);
        // Tiled stores rotation in degrees; engine convention is radians.
        spawned->SetRotation(obj.rotation * DekiMath::kDegToRad);
        if (obj.name[0]) spawned->SetName(obj.name);
        owner->AddChild(spawned);
        m_spawned.push_back(spawned);
    }
}
