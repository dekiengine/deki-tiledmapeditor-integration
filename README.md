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
- **TilemapColliderComponent** - exposes per-tile collision shapes from the
  tileset's collision objectgroups.
- **TilemapObjectSpawner** - instantiates engine scenes from Tiled object
  layers. Convention: a Tiled object with custom string property
  `scene_guid = "<guid>"` spawns that scene at the object's transform.

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

Scenes saved before 0.16.0 used bare names and still load; saving writes the current one.

## Dependencies

| Dependency | Type |
|---|---|
| `deki-2d` | Deki package |
| `deki-rendering` | Deki package |

## Install

Package Manager in the Deki Editor, or `DekiEditor --packages-add deki-tiledmapeditor-integration <project>`.

## License

Apache 2.0. See [LICENSE](LICENSE).

Third-party notices are listed in [NOTICE](NOTICE).
