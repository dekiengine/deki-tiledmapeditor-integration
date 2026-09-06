#pragma once

#ifdef DEKI_EDITOR

namespace DekiTilemap
{

// Registers .tmj/.tsj sync handlers with AssetPipeline. Idempotent —
// safe to call multiple times (only the first call subscribes).
void RegisterTilemapSyncHandlers();

} // namespace DekiTilemap

#endif // DEKI_EDITOR
