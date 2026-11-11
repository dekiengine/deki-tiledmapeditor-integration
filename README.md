# deki-tiledmapeditor-integration

Docs: https://dekiengine.github.io/deki-tiledmapeditor-integration/ (components and properties, generated from the code)

Tiled Map Editor (https://www.mapeditor.org) integration for the Deki engine.

Author tilemaps in Tiled, drop the exported `.tmj` (and any external `.tsj`
tilesets) into your project's `assets/` folder. The editor bakes them to a
fixed-layout binary (`.dtilemap` / `.dtileset`) optimised for streaming on
ESP32 SD cards and direct mmap-style reads on desktop.

## Features

- **TilemapComponent** - renders any layer set of a Tiled map through the
  shared `QuadBlit` pipeline used by `SpriteComponent`. Per-tile flip flags
  (H/V/D) are honored.
- **TilemapStreamer** - LRU chunk paging keyed by viewport, with a memory
  budget and a per-frame IO budget so a scroll never stalls on storage. The
  memory budget is the component's **Chunk Cache KiB**, 256 by default, which
  is sized for an ESP32; a desktop build can afford far more. Past the budget
  the least recently drawn chunks are dropped and re-read when they are needed
  again, so a budget that is too small shows up as stutter while scrolling
  rather than as missing tiles. Two objects drawing the same map settle on the
  larger of their two requests.
- **Every Tiled orientation** - orthogonal, isometric (diamonds, back to
  front, tall tiles standing on their squares), staggered and hexagonal (either
  stagger axis and index), laid out and drawn in Tiled's order.
- **TilemapColliderComponent** - exposes per-tile collision shapes from the
  tileset's collision objectgroups.
- **Layers as Tiled draws them** - opacity, tint, offsets, parallax and blend
  modes, on layers and groups; image layers; animated tiles; a tileset's Tile
  Render Size. An object layer's parallax moves its spawned objects with the
  view. An object layer's blend mode is not drawn, and baking says so.
- **TilemapObjectSpawner** - turns Tiled object layers into objects, in an
  object named "<owner> objects" beside its own, so they sort with the rest of
  the scene. A tile object gets a sprite of its tile, animated if its tile is,
  and **Sort Per Meter** sorts them by how low their points stand; set it to
  the Pixels Per Meter for one order per pixel row, which stacks them as the
  Tiled editor does. A tileset's Tile Offset moves the picture without
  changing the order. Tile and image layers above an object layer draw above
  its objects. `Tilemap::FindProperty` reads an object's custom properties.
  A Tiled object with custom string property `scene_guid = "<guid>"` spawns
  that scene at the object's transform.

## Source format

Only **JSON** Tiled exports are accepted: `.tmj` for maps, `.tsj` for tilesets.
Use Tiled's *Save As* to switch from the default `.tmx` format.

Layer data must be **uncompressed** (CSV or base64). In Tiled, set
*Map Properties → Tile Layer Format* to `CSV` or `Base64 (uncompressed)`.
Embedded tilesets and gzip/zlib/zstd chunk compression are rejected at bake
time with a clear error - no silent fallback.

## File layout

```
DekiTilemapPackage.{h,cpp}          DLL entry, plugin exports
Tilemap.{h,cpp}                    Runtime asset, .dtilemap loader
Tileset.{h,cpp}                    Runtime asset, .dtileset loader
TileChunk.h                        POD chunk struct
TilemapStreamer.{h,cpp}            LRU paging
TilemapComponent.{h,cpp}           Render component
TilemapColliderComponent.{h,cpp}   Collision component
TilemapObjectSpawner.{h,cpp}       Object-layer scene spawner
TilemapRenderSystem.{h,cpp}        Per-frame visibility + draw pass
editor/                            Editor-only code (sync handlers, baker, parser)
```

## Namespace

Types live in `DekiTiledMap`. Scene files store the qualified name, and so does code:

```cpp
using namespace DekiTiledMap;
obj->AddComponent<SomeComponent>();
```

## Dependencies

| Dependency | Type |
|---|---|
| `deki-2d` | Deki package |
| `deki-rendering` | Deki package |

## Install

Package Manager in the Deki Editor, or `DekiEditor --packages-add deki-tiledmapeditor-integration <project>`.

## Community

Questions, ideas and what you made with Deki: [the Deki Discord](https://dekiengine.com/discord/).

## License

Apache 2.0. See [LICENSE](LICENSE).

Third-party notices are listed in [NOTICE](NOTICE).
