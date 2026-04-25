#include "TilemapColliderComponent.h"

#include "DekiLogSystem.h"
#include "Tileset.h"
#include "TilemapStreamer.h"
#include "assets/AssetManager.h"

bool TilemapColliderComponent::HitTest(float worldX, float worldY, uint32_t* outLocalId)
{
    DekiTilemap::Tilemap* tm = tilemap.Get();
    if (!tm) return false;

    const int tw = tm->TileWidth();
    const int th = tm->TileHeight();
    if (tw <= 0 || th <= 0) return false;

    // World coords are in tile-pixels matching Tiled's coordinate system.
    const int tileX = static_cast<int>(worldX) / tw;
    const int tileY = static_cast<int>(worldY) / th;
    const int cw = tm->ChunkWidth();
    const int ch = tm->ChunkHeight();
    const int chunkX = (tileX < 0) ? -((-tileX + cw - 1) / cw) : tileX / cw;
    const int chunkY = (tileY < 0) ? -((-tileY + ch - 1) / ch) : tileY / ch;
    const int withinX = tileX - chunkX * cw;
    const int withinY = tileY - chunkY * ch;

    auto* streamer = tm->Streamer();
    if (!streamer)
    {
        DEKI_LOG_ERROR("TilemapCollider: no streamer attached to tilemap");
        return false;
    }
    const DekiTilemap::TileChunk* chunk = streamer->Get(collision_layer, chunkX, chunkY);
    if (!chunk)
    {
        DEKI_LOG_ERROR("TilemapCollider: chunk (%d,%d) layer %d is not resident — "
                       "request it via TilemapStreamer::RequestRect first",
                       chunkX, chunkY, collision_layer);
        return false;
    }

    const uint32_t gid = chunk->tileGids[withinY * cw + withinX];
    if (DekiTilemap::GidIndex(gid) == 0) return false;

    uint32_t localId = 0;
    const DekiTilemap::TilesetRef* tref = tm->ResolveTileset(gid, localId);
    if (!tref) return false;

    auto* tileset = Deki::AssetManager::Get()
        ? static_cast<DekiTilemap::Tileset*>(
              Deki::AssetManager::Get()->LoadByGuidAndType(tref->guid, DekiTilemap::Tileset::AssetTypeName))
        : nullptr;
    if (!tileset) return false;

    const DekiTilemap::DTileCollision* col = tileset->GetCollision(localId);
    if (!col) return false;

    // Local point inside the tile.
    const float px = worldX - tileX * tw;
    const float py = worldY - tileY * th;

    if (col->shape == static_cast<uint32_t>(DekiTilemap::DTileCollisionShape::Rect))
    {
        if (px >= col->x && px <= col->x + col->width &&
            py >= col->y && py <= col->y + col->height)
        {
            if (outLocalId) *outLocalId = localId;
            return true;
        }
        return false;
    }
    if (col->shape == static_cast<uint32_t>(DekiTilemap::DTileCollisionShape::Ellipse))
    {
        const float rx = col->width  * 0.5f;
        const float ry = col->height * 0.5f;
        if (rx <= 0.0f || ry <= 0.0f) return false;
        const float cx = col->x + rx;
        const float cy = col->y + ry;
        const float dx = (px - cx) / rx;
        const float dy = (py - cy) / ry;
        if (dx * dx + dy * dy <= 1.0f)
        {
            if (outLocalId) *outLocalId = localId;
            return true;
        }
        return false;
    }
    // Polygon collision: bounding-box test only in v1.
    if (px >= col->x && px <= col->x + col->width &&
        py >= col->y && py <= col->y + col->height)
    {
        if (outLocalId) *outLocalId = localId;
        return true;
    }
    return false;
}
