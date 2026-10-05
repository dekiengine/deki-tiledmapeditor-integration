#pragma once

#include <cstdint>
#include <vector>

#include <deki/Component.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace Deki
{
class Object;
}

namespace DekiTiledMap
{

// On Awake, spawns an object for each object in the tilemap's object layers:
//
//   - If the Tiled object has a "scene_guid" string property, that scene is
//     instantiated at the object's transform.
//   - Otherwise an empty Deki::Object with the Tiled object's name and
//     transform. Tile objects do not get a sprite yet.
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Spawns objects from a Tiled map's object layers when the scene loads.")
class TilemapObjectSpawner : public Deki::Component
{
public:
    DEKI_EXPORT
    DEKI_TOOLTIP("The map whose object layer is read. Each object placed in Tiled becomes an object in the scene.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    // Map pixels per world meter, to turn Tiled positions into meters. Should
    // match the TilemapComponent's pixelsPerMeter (default 16).
    DEKI_EXPORT
    DEKI_TOOLTIP("How many of the map's pixels make one meter, so spawned objects land where Tiled put them.")
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    void Awake() override;

private:
    std::vector<Deki::Object*> m_MSpawned;
};

}  // namespace DekiTiledMap
