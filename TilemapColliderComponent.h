#pragma once

#include <cstdint>

#include <deki/Behaviour.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

// Exposes per-tile collision shapes from a tilemap's tilesets. v1 query path
// is resident-chunks-only: hitting a non-resident chunk returns false and
// logs an error (no silent miss, per project policy).
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Exposes a tilemap layer's per-tile collision shapes for queries.")
class TilemapColliderComponent : public Deki::Behaviour
{
public:

    DEKI_EXPORT
    Deki::AssetRef<DekiTilemap::Tilemap> tilemap;

    // Layer index to source collision from. Default: layer 0.
    DEKI_EXPORT
    int32_t collisionLayer = 0;

    // Loop collision on each axis. When enabled, wrap_period controls the
    // strip size: 0 = auto (use authored bounds), >0 = explicit tile count.
    // Should match the rendering component.
    DEKI_EXPORT
    bool loopX = false;

    DEKI_EXPORT
    DEKI_VISIBLE_WHEN(loopX, 1)
    int32_t wrapPeriodX = 0;

    DEKI_EXPORT
    bool loopY = false;

    DEKI_EXPORT
    DEKI_VISIBLE_WHEN(loopY, 1)
    int32_t wrapPeriodY = 0;

    // Returns true if any tile at world position (x, y) carries a collision
    // shape that contains the point. outLocalId is set to the tile's local id
    // within its tileset on hit.
    bool HitTest(float worldX, float worldY, uint32_t* outLocalId = nullptr);
};

