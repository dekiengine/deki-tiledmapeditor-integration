# Changelog

Notable changes to `deki-tiledmapeditor-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Changed
- The map pass says when a map changed (deki-rendering's
  redraw-only-what-changed): where it sits, its settings, a chunk loaded or
  dropped (`TilemapStreamer::Changes`), a tileset arriving. A map still
  loading chunks counts as changed, so it keeps loading them.
- The map's render pass says which objects it draws (`WantsObject`): in strips
  it skips the rest, and a whole frame under a full-screen background is not
  cleared first (deki-rendering's lazy clear).
- **Maps draw faster.** A tile with see-through edges copies its opaque run
  and blends only the rest, from spans found once per tile, and a fully
  opaque tile in an atlas that has see-through tiles is drawn from a 2-byte
  copy instead of read 3 bytes a pixel: tiles take the same paths sprites do.
  Each column's and row's screen position is worked out once per chunk, and
  empty cells are skipped at once. The demo's meadow on a T-Deck Plus renders
  in 6.0 ms instead of 6.9 ms (6.6 ms when its tiles were sprites), with the
  same pixels.

### Fixed
- **Maps draw on boards.** A firmware links the package from a static
  library, which left out the render pass and the Tileset loader: nothing
  referred to their files, so their registrations never ran, and a map drew
  nothing, with no error. `DekiTilemapInitSystem()` now registers the loaders
  and the pass, and the firmware calls it (`PACKAGE_HAS_SYSTEM_INIT`); the
  editor calls it when it loads the package.
- **A build ships a map's tilesets and their images.** The package reports
  them to the editor (`AssetPipeline::RegisterDependencyReporter`), so a build
  for a board includes them; it used to ship the map alone.

## 0.19.0

### Changed
- `minEngine` 0.19.0: built against engine 0.19.0, whose component metadata records the package each class is in. A build of this package from before does not load and is rebuilt.

### Added
- Japanese, Simplified Chinese and Korean: `editor/lang/ja.json`, `zh.json`
  and `ko.json` translate the names, tooltips and enum values this
  package's components show in the Inspector.
- The package's editor text goes through the editor's translation (`Tr`), so
  it shows in the editor's language. Needs the editor release that adds
  `Localization.h`; raise `minEngine` to it when releasing.

## 0.18.0

### Changed
- `minEngine` 0.18.0. Reflection ABI 21: the package must be rebuilt.
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
