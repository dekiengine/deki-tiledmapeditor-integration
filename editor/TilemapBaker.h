#pragma once

#ifdef DEKI_EDITOR

#include <string>

#include "TmjParser.h"

namespace DekiTilemap
{

struct BakedTilesetRef
{
    uint32_t    firstGid;
    std::string guid;        // .dtileset GUID assigned by the sync handler
};

// Write a .dtileset binary for the given parsed TmjTileset.
//   atlasGuid   — GUID of the baked .dtex texture for the tileset's image
//   outAbsPath  — absolute output path (typically cache/<tilesetGuid>)
// Returns true on success, false on IO error.
bool WriteDtileset(const TmjTileset& ts,
                   const std::string& atlasGuid,
                   const std::string& outAbsPath);

// Write a .dtilemap binary for the given parsed TmjMap. Tileset references
// must be resolved (firstGid + GUID) by the caller; baker doesn't do file IO
// for tilesets.
bool WriteDtilemap(const TmjMap& map,
                   const std::vector<BakedTilesetRef>& tilesets,
                   const std::string& outAbsPath);

} // namespace DekiTilemap

#endif // DEKI_EDITOR
