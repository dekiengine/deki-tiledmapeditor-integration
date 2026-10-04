#pragma once

#include <cstdint>

#include <deki/Component.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace DekiTiledMap
{

// Answers point queries against the collision shapes in a tilemap's
// tilesets. Only loaded chunks can be queried: a point in a chunk that is not
// loaded returns false and logs an error, so a miss is never silent.
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Exposes a tilemap layer's per-tile collision shapes for queries.")
DEKI_FORMER_NAME("TilemapColliderComponent")
class TilemapColliderComponent : public Deki::Component
{
public:
    DEKI_EXPORT
    DEKI_TOOLTIP("The map whose tiles become solid.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    DEKI_EXPORT
    DEKI_TOOLTIP("Which layer of the map holds the collision tiles. A tile present on that layer is solid; an empty "
                 "cell is not.")
    int32_t collisionLayer = 0;

    // Loop collision on each axis. wrapPeriodX/Y set the repeat size: 0 uses
    // the map's bounds, more is a tile count. Should match the
    // TilemapComponent's settings.
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

    /// True if the tile at world position (x, y) has a collision shape that
    /// contains the point. On a hit, outLocalId gets the tile's local id in its
    /// tileset.
    bool HitTest(float worldX, float worldY, uint32_t* outLocalId = nullptr);
};

}  // namespace DekiTiledMap
