#pragma once

#include <cstdint>
#include <vector>

#include <deki/Component.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace Deki { class Object; }

namespace DekiTiledMap
{

// On Awake, walks Tilemap::ObjectLayers and spawns engine objects per Tiled
// object using the scene_guid convention:
//
//   - If a Tiled object has custom string property "scene_guid" set, that
//     scene is instantiated at the object's transform.
//   - Otherwise, if the object has a non-zero gid (tile object), an empty
//     Deki::Object with a Deki2D::SpriteComponent is spawned.
//   - Otherwise, an empty Deki::Object is spawned with name/type populated.
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Spawns objects from a Tiled map's object layers when the scene loads.")
DEKI_FORMER_NAME("TilemapObjectSpawner")
class TilemapObjectSpawner : public Deki::Component
{
public:

    DEKI_EXPORT
    DEKI_TOOLTIP("The map whose object layer is read. Each object placed in Tiled becomes an object in the scene.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    // Source pixels per world meter for Tiled object positions. Should match
    // the TilemapComponent's pixelsPerMeter (default 16). Used to divide
    // Tiled pixel coordinates into engine-world meters at spawn time.
    DEKI_EXPORT
    DEKI_TOOLTIP("How many of the map's pixels make one meter, so spawned objects land where Tiled put them.")
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    void Awake() override;

private:
    std::vector<Deki::Object*> m_MSpawned;
};

}  // namespace DekiTiledMap

