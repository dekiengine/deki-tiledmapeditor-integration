#pragma once

#include <cstdint>

#include "DekiBehaviour.h"
#include "Tilemap.h"
#include "assets/AssetRef.h"
#include "reflection/DekiProperty.h"

// Exposes per-tile collision shapes from a tilemap's tilesets. v1 query path
// is resident-chunks-only: hitting a non-resident chunk returns false and
// logs an error (no silent miss, per project policy).
class TilemapColliderComponent : public DekiBehaviour
{
public:
    DEKI_COMPONENT(TilemapColliderComponent, DekiBehaviour, "Tilemap",
                   "e2b8c4a1-9f30-4d65-8c11-7a4b3e2d1f88",
                   "DEKI_FEATURE_TILEMAP_COLLISION")

    DEKI_EXPORT
    Deki::AssetRef<DekiTilemap::Tilemap> tilemap;

    // Layer index to source collision from. Default: layer 0.
    DEKI_EXPORT
    int32_t collision_layer = 0;

    // Returns true if any tile at world position (x, y) carries a collision
    // shape that contains the point. outLocalId is set to the tile's local id
    // within its tileset on hit.
    bool HitTest(float worldX, float worldY, uint32_t* outLocalId = nullptr);
};

#include "generated/TilemapColliderComponent.gen.h"
