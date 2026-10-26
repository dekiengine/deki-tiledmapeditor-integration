#pragma once

/// Registers the Tilemap and Tileset asset loaders and the "tilemap" render
/// pass. Safe to call more than once.
///
/// Called from DekiInitPackageSystems() on firmware and static builds, and
/// from DekiPluginInit() when the package is loaded as a DLL. A firmware links
/// the package from a static library, where a file nothing refers to is left
/// out, and with it any registration it would have made by itself.
///
/// Must stay in the global namespace: the editor generates a file that
/// declares these as plain `extern void DekiTilemapInitSystem();`.
void DekiTilemapInitSystem();
void DekiTilemapShutdownSystem();
