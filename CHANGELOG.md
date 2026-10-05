# Changelog

Notable changes to `deki-tiledmapeditor-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Changed
- **Names follow the code style** (deki-engine/docs/codestyle): types, functions and enum values are PascalCase, constants kPascalCase, members m_PascalCase, locals and parameters camelCase. The code is formatted with clang-format 22.
- The functions the editor finds by name are PascalCase: DekiTilemapRegisterComponents, DekiTilemapGetAutoComponentCount, DekiTilemapEnsureRegistered and the rest. Built against engine ABI 21; a build of this package from before does not load and is rebuilt.
- Renamed: `AssetTypeName` is `kAssetTypeName`, `GID_INDEX_MASK` and the other gid flags are `kGidIndexMask`, ..., `CHUNK_FLAG_EMPTY` is `ChunkFlagEmpty`.

### Fixed
- The Chunk Padding tooltip says it counts chunks; it said tiles.
- **Builds for ESP32 boards again** (0.17.0 did not): the Max Size change
  called `std::max` with an `int` and an `int32_t`, which is `long` on Xtensa,
  and left two members of the tile lookup uninitialised, an error there.
- **Object properties load.** The baker wrote every object's properties, but
  the loader never read them back, so `Properties()` was always empty and a
  `TilemapObjectSpawner` never found an object's `scene_guid`. The map header
  now says where the properties, points and strings are. New: `Tilemap::ObjectProperties(object, count)`.
- **Polygon and polyline points are written.** Objects had a point count but
  the points themselves were never baked. `DTilemapObject::pointOffset` is now
  the object's first point in `PolygonPoints()`. Maps baked before this have
  no points until they are baked again.
- A map no longer reads its whole object list into memory twice.
- Corrupt map and tileset files are refused instead of crashing: every table
  is checked against the file's size, the per-layer object counts can no
  longer wrap and overflow the object table, chunk sides must be 1 to 1024
  (zero divided by zero in the collider, a huge one overflowed the chunk
  buffer on 32-bit boards), and a tileset's animation frame counts can no
  longer wrap.

### Removed
- Reading maps baked before the header said where the pools are. The editor
  bakes every map again (asset cache version 5).
- The former names from before 0.16.0 (bare class names, and deki-gpio's
  `DekiEsp32::ESP32PinSetup`). A scene that old is upgraded with 0.17 first.

## 0.17.0

### Changed
- `minEngine` 0.17.0. Reflection ABI 20: the package must be rebuilt.
- Max Size: tile rects (in the atlas image's pixels) are mapped to the
  atlas's stored pixels, and each tile is drawn at the tileset's size, so a
  shrunk atlas keeps its tiles and their size in the world.
- Tiles snap to the art-pixel grid when the project is Pixel Perfect.

## 0.16.0

### Fixed
- **The chunk memory budget is reachable.** `SetMemoryBudget` existed and
  nothing called it, so every target ran on the 256 KiB default chosen for an
  ESP32, desktop builds included. `TilemapComponent` now exposes it as **Chunk
  Cache KiB** and applies it when the map resolves. The budget belongs to the
  map, which the asset manager shares between components, so a component only
  ever raises it: two objects drawing one map settle on the larger request
  instead of the last one resolved.
- **Maps and tilesets load outside the editor.** Both loaders read the asset
  file with `std::fopen`. The asset manager prefixes the cache directory, which
  is the `S:/` mount on a device and in the desktop simulator, and only the
  engine's filesystem resolves that prefix — stdio sees a drive that does not
  exist. So a tilemap loaded in the editor, where the cache directory is a real
  native path, and nowhere else. Both now read through `IFileSystem`, which is
  what the chunk streamer set up at the end of `Tilemap::Load` had always done:
  one function was using both routes. Covered by tests that serve the file from
  a filesystem mounted at `S:/`.

### Changed
- **Moved into the `DekiTiledMap` namespace.** Every component was declared at global
  scope, which made its identity a bare class name — the name a scene file
  stores and the name the registry keys on — so two packages defining one name
  collided there with nothing to tell them apart. Each component carries
  `DEKI_FORMER_NAME` with the name it was saved under before, so existing
  scenes load unchanged and are written back qualified on the next save.
  Code naming these types needs the namespace: `using namespace DekiTiledMap;` or a
  qualified name.
- Enum properties are stored by name rather than by number, so appending to an
  enum or reordering one no longer changes what a saved scene means. Files
  written before this still read.
- `minEngine` 0.16.0. Reflection ABI 17: the package must be rebuilt.

## 0.15.0

### Changed
- Allocates through the engine, DMA buffers included, with an explicit region.
- Blit sources are built from a named `PixelLayout`, and `Texture2D` comes
  from the engine core.
