#pragma once

#ifdef DEKI_EDITOR

namespace DekiTiledMap
{

// Registers .tmj/.tsj sync handlers with AssetPipeline. Idempotent —
// safe to call multiple times (only the first call subscribes).
void RegisterTilemapSyncHandlers();

} // namespace DekiTiledMap

#endif // DEKI_EDITOR
