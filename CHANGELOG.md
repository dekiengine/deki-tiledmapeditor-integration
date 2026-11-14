# Changelog

Notable changes to `deki-tiledmapeditor-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## 0.40.0

### Fixed
- A map under a camera that is not a number, or very far out, draws nothing.
  Its chunk range wrapped round and `Tilemap::QueryVisibleChunks` read past
  the end of its index, which crashed. A range with its ends the wrong way
  round now returns no chunks.

### Changed
- `minEngine` 0.40.0: built against engine 0.40.0. A build of this package
  from before does not load and is rebuilt.
- **The view copy works with a Pixel Perfect camera on screen pixels.** A
  map drawn at 2x or more is still drawn from its copy when the camera's
  Pixel Grid is `Screen`, and scrolls a screen pixel at a time.

### Added
- **The view copy works on ARGB8888 screens and at 2x, 3x...** The copy a
  map is drawn into once, and copied from while the camera scrolls, was only
  made for RGB565 screens at 1:1, so a map on an ARGB8888 screen, or drawn
  larger under Pixel Perfect, was drawn tile by tile every frame. On ARGB8888
  it is kept in the screen's format and copied row by row, where the map is
  the first thing drawn; under Pixel Perfect at a whole scale its tiles land
  on the same art-pixel grid as on screen. Nothing to set, and the same
  pixels: frames rendered both ways match. On the Anbernic RG40XX H (640x480,
  a four-layer isometric farm at 2x) the map went from 13 ms a frame to 2.6.
  The log says once when a map is drawn from a copy.
- The map's render pass names itself "Tilemap" in the profiler's breakdown
  by object (`RenderPass::Name`).
- Isometric maps. Set the map's orientation to Isometric in Tiled and it draws
  as diamonds, back to front across chunks, with tall tiles standing on their
  squares. Only chunks in view are loaded. Collision and spawned objects follow
  the same layout.
- Tile objects placed in Tiled now appear. `TilemapObjectSpawner` gives each
  one a sprite of its tile, standing where Tiled draws it, with its flips,
  size and rotation. **Sort Base** and **Sort Per Meter** set its Sorting
  Order from where its point stands, so what stands lower on the screen can
  draw on top. A Tile Offset moves the picture without changing the order.
- `Tilemap::FindProperty`, which finds one of an object's custom properties
  by name.
- Tilesets with a margin or spacing. These were read as if they had none,
  which showed the wrong part of the image.
- A tileset's Tile Offset and Object Alignment.
- Group layers. Their layers used to be left out.
- A layer's opacity, tint, offset and parallax, and a group's, drawn as in
  Tiled. An object layer's opacity, tint and offset apply to its objects.
- Image layers, with their repeat across and down.
- Animated tiles play, in tile layers and as tile objects, on one clock as in
  Tiled.
- Tile and image layers above an object layer draw above its objects, as
  Tiled stacks them: the spawner draws them from components of their own.
- Staggered and hexagonal maps, with either stagger axis and index and a hex
  side length, laid out and drawn in Tiled's order. Collision, spawned
  objects and `Tilemap::PixelsToTile` follow the same layout;
  `Tilemap::Stagger()` gives the grid.
- A tileset's Tile Render Size set to Map Grid Size, with Fill Mode Stretch or
  Preserve Aspect Fit: its tiles are drawn at the map's tile size in tile
  layers.
- Layer blend modes (Add, Multiply, Screen, Overlay, Darken, Lighten, Color
  Dodge, Color Burn, Hard Light, Soft Light, Difference, Exclusion) on tile and
  image layers, a group's too, blended as Tiled blends them. A blended layer
  costs two extra draws and a blend for each of its tiles. An object layer's
  blend mode is not drawn, and baking says so.
- An object layer's parallax: its spawned objects go in an object named
  after the layer, which moves with the view before each frame as Tiled moves
  the layer.

### Changed
- A map that scrolls with the camera works out its copy once a frame. A frame
  redrawn in parts asked the map to draw once for each part, and each time
  the map found its layers, tilesets, box and copy again; now the frame's
  first part records what it found and the others only copy their pixels.
  With 16 sprites moving over the map on a LilyGO T-Deck that was about 15
  parts a frame.- Tile layers draw faster, with the same pixels. Each tile gets the runs a
  sprite gets: its rows' transparent runs are skipped unread and its runs of
  one colour are filled. Only the cells that can reach the frame (or the
  strip being drawn) are walked. The tables they need live in external
  memory, so a board's internal RAM stays for its display and frame. On a
  LilyGO T-Deck at 320x240, a whole frame of an isometric map went from 34 to
  24 ms, a staggered one from 38 to 25 ms and the "Your first game" farm from
  38 to 33 ms. A layer with a blend mode draws in about half the time it first
  did. With the project's `redrawChanges` on, a map follows a camera that
  scrolls by whole pixels: only the edge it uncovers is drawn, and the same
  three maps hold 60 fps (16.6 ms, the display's own rate). That needs the map
  at the project's pixels per meter, on whole pixels, with parallax 1 and whole
  layer offsets; any other map redraws the whole frame as before. A layer
  walks only the cells its own tiles can reach the drawn area from, so a
  map's tall building tiles no longer widen the walk of its ground (the
  guide's farm: about 5200 cells walked a frame down to 900), and chunks
  asked for once in a frame are not asked for again by its later parts.
- **A map that scrolls with the camera keeps a copy of its pixels.** Each
  group of layers a map draws together is kept, as drawn, in a buffer a margin
  larger than the view, in external memory. While the camera moves by whole
  pixels a frame, or each part redrawn of one, copies the map from there
  instead of drawing every layer's tiles again, and only what comes into view
  is drawn, a band at a time, at most one band piece a frame. Where the map is
  the first thing drawn, the copy holds it over the clear colour and the
  clear is skipped. The pixels are the ones drawing gives, checked pixel for
  pixel against drawing on a LilyGO T-Deck over every bench map. On the
  T-Deck the "Your first game" farm went from 31.7 to about 22.5 ms a frame
  with the camera following the farmer, and from 53.6 to 34.5 ms with 16
  villagers walking. A layer group is drawn as before, every frame, when it
  has a tint, opacity, blend mode or parallax, animated tiles, tiles with
  partly see-through pixels, or a map scaled from its pixels per meter, in
  frames drawn in strips, on a GPU or not in RGB565, with the camera between
  pixels, and when
  there is no memory for the copy (about 190 KB at 320x240). A map draws
  nothing in the parts of a frame away from all its tiles.
- An isometric map used to be drawn as a square grid and now draws correctly.
  One baked before this keeps the old layout until its `.tmj` is saved again.
- Object positions and sizes keep Tiled's fractions (`DTilemapObject::x`, `y`,
  `width` and `height` are floats). They were rounded down to whole pixels,
  so an object at 10.5 now lands half a pixel further on. Maps are baked as
  file version 3; versions 1 and 2 still load, as whole pixels.
- Objects spawned from a Tiled map keep the rotation drawn in Tiled, now that
  the engine draws positive rotation counter-clockwise (`deki-rendering`).
- On an isometric map, tiles wider than the grid line up with the left corner
  of their square, as in Tiled.
- A layer hidden in Tiled is no longer drawn, and collision can still read it.
  It used to be drawn. Show the layer in Tiled to keep it on screen.
- On a square map, tiles taller than the grid stand on the bottom of their
  cell, as in Tiled. They used to hang down from its top.
- On a square map, spawned objects land where Tiled shows them: relative to
  the map's centre, with Y up. They used to be off by the map's size and
  upside down, so anything placed to make up for that needs moving back.
- A tile object used to spawn as an empty object and now also has a sprite,
  unless it is hidden in Tiled.
- A tileset baked before this keeps ignoring its margin and spacing until its
  `.tsj` is saved again.
- Spawned objects go into an object named "<owner> objects" beside the
  spawner's object, not under it. Under an object that draws the map they were
  drawn with it, so a character could never walk behind them. Code that looked
  for them among the map object's children needs to look there.
- A map baked before this draws without its layers' opacity, tint, offsets,
  parallax and image layers until its `.tmj` is saved again (file version 2).
- **Sort Per Meter** orders are floored, so each step is one order around 0
  too, where they used to be cut toward 0 and the two steps either side of 0
  shared an order.

## 0.30.0

### Changed
- `minEngine` 0.30.0: built against engine 0.30.0. A build of this package
  from before does not load and is rebuilt.

### Added
- Tiles carry their tileset image's filter (`QuadBlit::Source::filter`), so a
  GPU draws them as its Filter says, each tile within its own texels.
- Tiles carry their atlas's revision (`QuadBlit::Source::revision`), and each
  opaque tile copy one of its own, so a GPU drawing the 2D (`deki-gl`)
  uploads each tile once. The map pass says it writes nothing into the frame
  buffer itself (`RenderPass::WritesBuffer`).

## 0.22.0

### Changed
- Translations are keyed by the editor's names with their acronyms ("I2C Port",
  "SPI Host" rather than "I 2c Port", "Spi Host"), which the 0.22 editor shows.
- `minEngine` 0.22.0: built against engine 0.22.0. A build of this package
  from before does not load and is rebuilt.

### Added
- Tilemaps and tilesets have an inspector: size, tiles, layers and tilesets
  read from the file, and Open in Tiled.

## 0.21.0

### Changed
- `minEngine` 0.21.0: built against engine 0.21.0. A build of this package
  from before does not load and is rebuilt.
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
