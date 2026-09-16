#pragma once

#include <cstdint>

#include <deki/Component.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace DekiTiledMap
{

// Exposes per-tile collision shapes from a tilemap's tilesets. v1 query path
// is resident-chunks-only: hitting a non-resident chunk returns false and
// logs an error (no silent miss, per project policy).
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Exposes a tilemap layer's per-tile collision shapes for queries.")
DEKI_FORMER_NAME("TilemapColliderComponent")
class TilemapColliderComponent : public Deki::Component
{
public:

    DEKI_EXPORT
    DEKI_TOOLTIP("The map whose tiles become solid.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    // Layer index to source collision from. Default: layer 0.
    DEKI_EXPORT
    DEKI_TOOLTIP("Which layer of the map holds the collision tiles. A tile present on that layer is solid; an empty cell is not.")
    int32_t collisionLayer = 0;

    // Loop collision on each axis. When enabled, wrap_period controls the
    // strip size: 0 = auto (use authored bounds), >0 = explicit tile count.
    // Should match the rendering component.
    DEKI_EXPORT
    DEKI_TOOLTIP("Repeat the collision horizontally, to match a map that loops.")
    bool loopX = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("How wide one horizontal repeat is, in tiles. 0 uses the map's own width.")
    DEKI_VISIBLE_WHEN(loopX, 1)
    int32_t wrapPeriodX = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Repeat the collision vertically.")
    bool loopY = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("How tall one vertical repeat is, in tiles. 0 uses the map's own height.")
    DEKI_VISIBLE_WHEN(loopY, 1)
    int32_t wrapPeriodY = 0;

    // Returns true if any tile at world position (x, y) carries a collision
    // shape that contains the point. outLocalId is set to the tile's local id
    // within its tileset on hit.
    bool HitTest(float worldX, float worldY, uint32_t* outLocalId = nullptr);
};

}  // namespace DekiTiledMap

