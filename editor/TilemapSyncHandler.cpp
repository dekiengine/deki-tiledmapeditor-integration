#ifdef DEKI_EDITOR

#include "TilemapSyncHandler.h"
#include "TmjParser.h"
#include "TilemapBaker.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "DekiLogSystem.h"
#include "Guid.h"
#include "assets/AssetManager.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/SubAsset.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiTilemap
{

namespace
{

bool s_Registered = false;

std::string CacheDir(DekiEditor::AssetPipeline* pipeline)
{
    return (fs::path(pipeline->GetProjectPath()) / "cache").string();
}

// Compute a deterministic GUID for a sub-asset given a parent GUID + suffix.
std::string SubGuid(const std::string& parentGuid, const std::string& suffix)
{
    return Deki::GenerateDeterministicGuid(parentGuid + ":" + suffix);
}

// Look up the GUID assigned to an arbitrary asset path. Returns empty if not
// imported yet (caller should fail loudly).
std::string GuidForRelativePath(DekiEditor::AssetPipeline* pipeline, const std::string& rel)
{
    const auto* info = pipeline->GetAssetInfo(rel);
    return info ? info->guid : std::string();
}

void HandleTilesetSync(const std::string& absPath,
                       const std::string& guid,
                       const std::string& projectPath)
{
    auto* pipeline = DekiEditor::AssetPipeline::Instance();
    if (!pipeline)
    {
        DEKI_LOG_ERROR("TilesetSync: no active asset pipeline");
        return;
    }

    TmjTileset ts;
    std::string err;
    if (!ParseTsjTileset(absPath, ts, err))
    {
        DEKI_LOG_ERROR("TilesetSync: parse failed for '%s': %s", absPath.c_str(), err.c_str());
        return;
    }

    // Resolve the atlas image's GUID. The image is referenced relative to the .tsj
    // file location.
    fs::path tsjPath = absPath;
    fs::path imagePath = (tsjPath.parent_path() / ts.imageRelative).lexically_normal();
    fs::path imageRel  = fs::relative(imagePath, projectPath);
    std::string imageRelStr;
    {
        // Normalize to forward slashes — AssetPipeline keys are forward-slash.
        imageRelStr = imageRel.generic_string();
    }
    std::string atlasGuid = GuidForRelativePath(pipeline, imageRelStr);
    if (atlasGuid.empty())
    {
        DEKI_LOG_ERROR("TilesetSync: tileset image '%s' has not been imported by the editor yet "
                       "(referenced from '%s')", imageRelStr.c_str(), absPath.c_str());
        return;
    }

    // Cache output: cache/<guid>
    fs::path outPath = fs::path(CacheDir(pipeline)) / guid;
    if (!WriteDtileset(ts, atlasGuid, outPath.string()))
    {
        DEKI_LOG_ERROR("TilesetSync: bake failed for '%s'", absPath.c_str());
        return;
    }

    Deki::AssetManager::Get()->RegisterGuid(guid, guid);
    DEKI_LOG_EDITOR("TilesetSync: baked '%s' -> %s (atlas=%s)",
                    absPath.c_str(), guid.c_str(), atlasGuid.c_str());
}

void HandleTilemapSync(const std::string& absPath,
                       const std::string& guid,
                       const std::string& projectPath)
{
    auto* pipeline = DekiEditor::AssetPipeline::Instance();
    if (!pipeline)
    {
        DEKI_LOG_ERROR("TilemapSync: no active asset pipeline");
        return;
    }

    TmjMap map;
    std::string err;
    if (!ParseTmjMap(absPath, map, err))
    {
        DEKI_LOG_ERROR("TilemapSync: parse failed for '%s': %s", absPath.c_str(), err.c_str());
        return;
    }

    // For each external tileset reference, ensure the .tsj has been imported
    // and pull its GUID. Do NOT recursively sync — AssetPipeline schedules
    // .tsj handlers itself when those files exist in the project.
    fs::path tmjPath = absPath;
    std::vector<BakedTilesetRef> baked;
    std::vector<DekiEditor::SubAssetInfo> subs;
    int subIdx = 0;
    for (const auto& tref : map.tilesets)
    {
        fs::path tsjAbs = (tmjPath.parent_path() / tref.source).lexically_normal();
        fs::path tsjRel = fs::relative(tsjAbs, projectPath);
        std::string tsjRelStr = tsjRel.generic_string();
        std::string tsGuid = GuidForRelativePath(pipeline, tsjRelStr);
        if (tsGuid.empty())
        {
            DEKI_LOG_ERROR("TilemapSync: external tileset '%s' has no GUID — make sure the .tsj "
                           "is in the project's assets directory", tsjRelStr.c_str());
            return;
        }

        baked.push_back({tref.firstGid, tsGuid});

        // Register a sub-asset so the asset browser shows the tileset under
        // the map.
        DekiEditor::SubAssetInfo s;
        s.guid          = tsGuid;
        s.parentGuid    = guid;
        s.subAssetIndex = subIdx++;
        s.name          = tsjAbs.stem().string();
        s.depth         = 0;
        s.hasPreview    = true;
        subs.push_back(s);
    }

    fs::path outPath = fs::path(CacheDir(pipeline)) / guid;
    if (!WriteDtilemap(map, baked, outPath.string()))
    {
        DEKI_LOG_ERROR("TilemapSync: bake failed for '%s'", absPath.c_str());
        return;
    }

    pipeline->RegisterSubAssets(guid, subs);

    // Update .data sidecar to record the resolved tileset GUIDs (for tooling /
    // hot-reload diff).
    fs::path dataPath = absPath + std::string(".data");
    json sidecar;
    if (fs::exists(dataPath))
    {
        std::ifstream in(dataPath);
        try { sidecar = json::parse(in); } catch (...) { sidecar = json{}; }
    }
    sidecar["baked"] = true;
    json& tilesets = sidecar["tilesets"];
    tilesets = json::object();
    for (const auto& b : baked)
        tilesets[std::to_string(b.firstGid)] = json{{"guid", b.guid}};
    {
        std::ofstream out(dataPath);
        out << sidecar.dump(2);
    }

    Deki::AssetManager::Get()->RegisterGuid(guid, guid);
    DEKI_LOG_EDITOR("TilemapSync: baked '%s' -> %s (%zu tilesets, %zu layers)",
                    absPath.c_str(), guid.c_str(),
                    baked.size(), map.tileLayers.size());
}

} // namespace

void RegisterTilemapSyncHandlers()
{
    if (s_Registered) return;
    s_Registered = true;

    DekiEditor::AssetPipeline::OnStarted([](DekiEditor::AssetPipeline* p) {
        // .tsj must be processed before .tmj — AssetPipeline doesn't guarantee
        // ordering, so HandleTilemapSync just checks for the import and fails
        // loudly if the .tsj hasn't been registered yet. The sidecar baked-flag
        // mechanism re-runs on next scan once both are present.
        p->RegisterSyncHandler(".tsj", HandleTilesetSync);
        p->RegisterSyncHandler(".tmj", HandleTilemapSync);
    });
}

} // namespace DekiTilemap

#endif // DEKI_EDITOR
