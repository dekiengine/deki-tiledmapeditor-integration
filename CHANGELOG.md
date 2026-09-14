# Changelog

Notable changes to `deki-tiledmapeditor-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## 0.15.0

### Changed
- Allocates through the engine, DMA buffers included, with an explicit region.
- Blit sources are built from a named `PixelLayout`, and `Texture2D` comes
  from the engine core.
