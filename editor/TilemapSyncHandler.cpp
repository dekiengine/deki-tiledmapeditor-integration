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

bool s_SyncHandlerRegistered = false;

// Look up the GUID assigned to an arbitrary asset path. Returns empty if the
// file does not exist on disk; otherwise reads/creates the .data sidecar so the
// lookup does not depend on directory iteration order — m_Assets is only
// populated as ProcessAsset visits each file, and a .tmj can be visited before
// its referenced .tsj/.png.
std::string GuidForRelativePath(DekiEditor::AssetPipeline* pipeline, const std::string& rel)
{
    fs::path abs = fs::path(pipeline->GetAbsolutePath(rel));
    if (!fs::exists(abs))
        return std::string();
    return pipeline->GetOrCreateAssetGuid(rel);
}

DekiEditor::AssetCacheResult HandleTilesetCache(const DekiEditor::AssetCacheContext& ctx)
{
    TmjTileset ts;
    std::string err;
    if (!ParseTsjTileset(ctx.absolutePath, ts, err))
    {
        DEKI_LOG_ERROR("TilesetSync: parse failed for '%s': %s", ctx.absolutePath.c_str(), err.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    // Resolve the atlas image's GUID. The image is referenced relative to the .tsj
    // file location.
    fs::path tsjPath = ctx.absolutePath;
    fs::path imagePath = (tsjPath.parent_path() / ts.imageRelative).lexically_normal();
    fs::path imageRel  = fs::relative(imagePath, ctx.projectPath);
    std::string imageRelStr = imageRel.generic_string();

    std::string atlasGuid = GuidForRelativePath(ctx.pipeline, imageRelStr);
    if (atlasGuid.empty())
    {
        DEKI_LOG_ERROR("TilesetSync: tileset image '%s' has not been imported by the editor yet "
                       "(referenced from '%s')", imageRelStr.c_str(), ctx.absolutePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    if (!WriteDtileset(ts, atlasGuid, ctx.cachePath))
    {
        DEKI_LOG_ERROR("TilesetSync: bake failed for '%s' (cache path '%s')",
                       ctx.absolutePath.c_str(), ctx.cachePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    // Belt-and-suspenders: also register the GUID -> path entry directly with
    // AssetManager. EditorProjectManager::OpenProject does this in its
    // post-ImportAllAssets loop, but only the *first* time the project opens.
    // Hot-reloading the module DLL re-runs our cache handler without re-running
    // OpenProject — without this call, AssetRef::Get() can't resolve the cache
    // path until the editor restarts.
    Deki::AssetManager::Get()->RegisterGuid(ctx.guid, ctx.guid);

    DEKI_LOG_EDITOR("TilesetSync: baked '%s' -> %s (atlas=%s)",
                    ctx.absolutePath.c_str(), ctx.guid.c_str(), atlasGuid.c_str());
    return DekiEditor::AssetCacheResult::Cached;
}

DekiEditor::AssetCacheResult HandleTilemapCache(const DekiEditor::AssetCacheContext& ctx)
{
    TmjMap map;
    std::string err;
    if (!ParseTmjMap(ctx.absolutePath, map, err))
    {
        DEKI_LOG_ERROR("TilemapSync: parse failed for '%s': %s", ctx.absolutePath.c_str(), err.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    // For each external tileset reference, ensure the .tsj has been imported
    // and pull its GUID. Do NOT recursively sync — AssetPipeline schedules
    // .tsj handlers itself when those files exist in the project.
    fs::path tmjPath = ctx.absolutePath;
    std::vector<BakedTilesetRef> baked;
    std::vector<DekiEditor::SubAssetInfo> subs;
    int subIdx = 0;
    for (const auto& tref : map.tilesets)
    {
        fs::path tsjAbs = (tmjPath.parent_path() / tref.source).lexically_normal();
        fs::path tsjRel = fs::relative(tsjAbs, ctx.projectPath);
        std::string tsjRelStr = tsjRel.generic_string();

        // .tsx (XML) tilesets aren't supported by this module — JSON only.
        // Tiled defaults to .tsx even when maps are .tmj, so this is the most
        // common bake failure. Give the user the exact fix.
        std::string ext = tsjAbs.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".tsx")
        {
            DEKI_LOG_ERROR(
                "TilemapSync: '%s' references XML tileset '%s'. The deki-tilemap module is "
                "JSON-only. In Tiled: open the .tsx, File > Export As > Tiled JSON Tileset (.tsj), "
                "then update the map's tileset reference to the .tsj file. To stop hitting this: "
                "Edit > Preferences > General > Store tilesets as > JSON.",
                ctx.absolutePath.c_str(), tref.source.c_str());
            return DekiEditor::AssetCacheResult::NotCached;
        }

        std::string tsGuid = GuidForRelativePath(ctx.pipeline, tsjRelStr);
        if (tsGuid.empty())
        {
            DEKI_LOG_ERROR(
                "TilemapSync: external tileset '%s' (referenced from '%s') has no GUID — "
                "the .tsj file must live somewhere under the project's assets/ folder so the "
                "editor can import it.",
                tsjRelStr.c_str(), ctx.absolutePath.c_str());
            return DekiEditor::AssetCacheResult::NotCached;
        }

        baked.push_back({tref.firstGid, tsGuid});

        // Register a sub-asset so the asset browser shows the tileset under
        // the map.
        DekiEditor::SubAssetInfo s;
        s.guid          = tsGuid;
        s.parentGuid    = ctx.guid;
        s.subAssetIndex = subIdx++;
        s.name          = tsjAbs.stem().string();
        s.depth         = 0;
        s.hasPreview    = true;
        subs.push_back(s);
    }

    if (!WriteDtilemap(map, baked, ctx.cachePath))
    {
        DEKI_LOG_ERROR("TilemapSync: bake failed for '%s' (cache path '%s')",
                       ctx.absolutePath.c_str(), ctx.cachePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    ctx.pipeline->RegisterSubAssets(ctx.guid, subs);

    // Belt-and-suspenders: register the GUID -> path entry directly. See the
    // matching comment in HandleTilesetCache for why this is needed despite
    // EditorProjectManager's post-import loop. Also register the export-path
    // key so AssetManager::Load<Tilemap>("path/to/test-map") works the same
    // way Sprite/BitmapFont do.
    Deki::AssetManager::Get()->RegisterGuid(ctx.guid, ctx.guid);

    // Update .data sidecar to record the resolved tileset GUIDs (for tooling /
    // hot-reload diff).
    fs::path dataPath = ctx.absolutePath + std::string(".data");
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

    DEKI_LOG_EDITOR("TilemapSync: baked '%s' -> %s (%zu tilesets, %zu layers)",
                    ctx.absolutePath.c_str(), ctx.guid.c_str(),
                    baked.size(), map.tileLayers.size());
    return DekiEditor::AssetCacheResult::Cached;
}

} // namespace

void RegisterTilemapSyncHandlers()
{
    if (s_SyncHandlerRegistered) return;
    s_SyncHandlerRegistered = true;

    DekiEditor::AssetPipeline::OnStarted([](DekiEditor::AssetPipeline* p) {
        // Cache handlers (not sync handlers): returning AssetCacheResult::Cached
        // sets info.hasCachedVersion=true, which makes EditorProjectManager's
        // post-import RegisterGuid loop see the asset and wire the GUID -> path
        // entry that AssetRef::Get() needs at runtime. Sync handlers can't do
        // this — they run after hasCachedVersion is already final.
        p->RegisterCacheHandler(".tsj", HandleTilesetCache);
        p->RegisterCacheHandler(".tmj", HandleTilemapCache);
    });
}

} // namespace DekiTilemap

#endif // DEKI_EDITOR
