#pragma once

#ifdef DEKI_EDITOR

#include <string>

#include "TmjParser.h"

namespace DekiTiledMap
{

struct BakedTilesetRef
{
    uint32_t firstGid;
    std::string guid;  // .dtileset GUID assigned by the sync handler
};

/// Writes a .dtileset file for a parsed tileset. `atlasGuid` is the GUID of
/// the baked .dtex of the tileset's image; `outAbsPath` is the absolute output
/// path (usually cache/<tilesetGuid>). Returns false on an IO error.
bool WriteDtileset(const TmjTileset& ts, const std::string& atlasGuid, const std::string& outAbsPath);

/// Writes a .dtilemap file for a parsed map. The caller resolves the tileset
/// references (firstGid and GUID); the baker reads no tileset files.
bool WriteDtilemap(const TmjMap& map, const std::vector<BakedTilesetRef>& tilesets, const std::string& outAbsPath);

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
