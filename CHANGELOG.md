# Changelog

Notable changes to `deki-tiledmapeditor-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## 0.16.0

### Fixed
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
