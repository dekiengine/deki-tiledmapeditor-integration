# Module descriptor for deki-engine auto-discovery
set(MODULE_DISPLAY_NAME "Tiled Map")
set(MODULE_PREFIX "DekiTilemap")
set(MODULE_UPPER "TILEDMAP")
set(MODULE_TARGET "deki-tiledmap")
set(MODULE_FILE_PREFIX "Tilemap")
set(MODULE_ENTRY DekiTilemapModule.cpp)
set(MODULE_LINK_DEPS deki-2d deki-rendering deki-editor)
set(MODULE_NEEDS_IMGUI ON)
