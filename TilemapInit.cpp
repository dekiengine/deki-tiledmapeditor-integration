#include "TilemapInit.h"
#include "Tilemap.h"
#include "TilemapRenderSystem.h"
#include "Tileset.h"

// Global scope, matching TilemapInit.h - see the comment there.
void DekiTilemapInitSystem()
{
    DekiTiledMap::RegisterTilemapLoader();
    DekiTiledMap::RegisterTilesetLoader();
    DekiTiledMap::RegisterTilemapRenderPass();
}

void DekiTilemapShutdownSystem()
{
}
