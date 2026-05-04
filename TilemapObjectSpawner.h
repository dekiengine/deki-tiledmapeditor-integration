#pragma once

#include <cstdint>
#include <vector>

#include "DekiBehaviour.h"
#include "Tilemap.h"
#include "assets/AssetRef.h"
#include "reflection/DekiProperty.h"

class DekiObject;

// On Awake, walks Tilemap::ObjectLayers and spawns engine objects per Tiled
// object using the prefab_guid convention:
//
//   - If a Tiled object has custom string property "prefab_guid" set, that
//     prefab is instantiated at the object's transform.
//   - Otherwise, if the object has a non-zero gid (tile object), an empty
//     DekiObject with a SpriteComponent is spawned.
//   - Otherwise, an empty DekiObject is spawned with name/type populated.
class TilemapObjectSpawner : public DekiBehaviour
{
public:
    DEKI_COMPONENT(TilemapObjectSpawner, DekiBehaviour, "Tilemap",
                   "a4f2b6c8-5d10-4e93-bc77-2f8a9d3c4e15",
                   "DEKI_FEATURE_TILEMAP_OBJECTS")

    DEKI_EXPORT
    Deki::AssetRef<DekiTilemap::Tilemap> tilemap;

    // Source pixels per world meter for Tiled object positions. Should match
    // the TilemapComponent's pixels_per_meter (default 16). Used to divide
    // Tiled pixel coordinates into engine-world meters at spawn time.
    DEKI_EXPORT
    DEKI_RANGE(1.0f, 256.0f)
    float pixels_per_meter = 16.0f;

    void Awake() override;
    bool NeedsUpdate() const override { return false; }

private:
    std::vector<DekiObject*> m_spawned;
};

#include "generated/TilemapObjectSpawner.gen.h"
