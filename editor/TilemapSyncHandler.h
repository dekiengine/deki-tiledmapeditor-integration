#pragma once

#ifdef DEKI_EDITOR

namespace DekiTiledMap
{

/// Registers the .tmj and .tsj sync handlers with the AssetPipeline. Safe to
/// call again; only the first call subscribes.
void RegisterTilemapSyncHandlers();

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
