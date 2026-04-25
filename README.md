# deki-tiledmap

Tiled Map Editor (https://www.mapeditor.org) integration for the Deki engine.

Author tilemaps in Tiled, drop the exported `.tmj` (and any external `.tsj`
tilesets) into your project's `assets/` folder. The editor bakes them to a
fixed-layout binary (`.dtilemap` / `.dtileset`) optimised for streaming on
ESP32 SD cards and direct mmap-style reads on desktop.

## Features

- **TilemapComponent** — renders any layer set of a Tiled map through the
  shared `QuadBlit` pipeline used by `SpriteComponent`. Per-tile flip flags
  (H/V/D) are honored.
- **TilemapStreamer** — LRU chunk paging keyed by viewport. Bounded memory
  budget (256 KiB ESP32 / 16 MiB desktop default), per-frame IO budget
  prevents stalls.
- **TilemapColliderComponent** — exposes per-tile collision shapes from the
  tileset's collision objectgroups.
- **TilemapObjectSpawner** — instantiates engine prefabs from Tiled object
  layers. Convention: a Tiled object with custom string property
  `prefab_guid = "<guid>"` spawns that prefab at the object's transform.

## Source format

Only **JSON** Tiled exports are accepted: `.tmj` for maps, `.tsj` for tilesets.
Use Tiled's *Save As* to switch from the default `.tmx` format.

Layer data must be **uncompressed** (CSV or base64). In Tiled, set
*Map Properties → Tile Layer Format* to `CSV` or `Base64 (uncompressed)`.
Embedded tilesets and gzip/zlib/zstd chunk compression are rejected at bake
time with a clear error — no silent fallback.

## File layout

```
DekiTilemapModule.{h,cpp}          DLL entry, plugin exports
Tilemap.{h,cpp}                    Runtime asset, .dtilemap loader
Tileset.{h,cpp}                    Runtime asset, .dtileset loader
TileChunk.h                        POD chunk struct
TilemapStreamer.{h,cpp}            LRU paging
TilemapComponent.{h,cpp}           Render component
TilemapColliderComponent.{h,cpp}   Collision component
TilemapObjectSpawner.{h,cpp}       Object-layer prefab spawner
TilemapRenderSystem.{h,cpp}        Per-frame visibility + draw pass
editor/                            Editor-only code (sync handlers, baker, parser)
```

## License

MIT. See LICENSE.
