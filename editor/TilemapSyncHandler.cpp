#ifdef DEKI_EDITOR

#include "TilemapSyncHandler.h"
#include "TmjParser.h"
#include "TilemapBaker.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include <deki/LogSystem.h>
#include <deki/Guid.h>
#include <deki/assets/AssetManager.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/SubAsset.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiTiledMap
{

namespace
{

bool s_SyncHandlerRegistered = false;

// The GUID of any asset path, or empty if the file does not exist. Reads or
// creates the .data sidecar, so the answer does not depend on the order files
// are visited: the pipeline learns of each file only as ProcessAsset reaches
// it, and a .tmj can come before the .tsj and .png it uses.
std::string GuidForRelativePath(DekiEditor::AssetPipeline* pipeline, const std::string& rel)
{
    fs::path abs = fs::path(pipeline->GetAbsolutePath(rel));
    if (!fs::exists(abs))
    {
        return std::string();
    }
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

    // The atlas image's GUID. Its path is relative to the .tsj.
    fs::path tsjPath = ctx.absolutePath;
    fs::path imagePath = (tsjPath.parent_path() / ts.imageRelative).lexically_normal();
    fs::path imageRel = fs::relative(imagePath, ctx.projectPath);
    std::string imageRelStr = imageRel.generic_string();

    std::string atlasGuid = GuidForRelativePath(ctx.pipeline, imageRelStr);
    if (atlasGuid.empty())
    {
        DEKI_LOG_ERROR("TilesetSync: tileset image '%s' has not been imported by the editor yet "
                       "(referenced from '%s')",
                       imageRelStr.c_str(), ctx.absolutePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    if (!WriteDtileset(ts, atlasGuid, ctx.cachePath))
    {
        DEKI_LOG_ERROR("TilesetSync: bake failed for '%s' (cache path '%s')", ctx.absolutePath.c_str(),
                       ctx.cachePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    // Register the GUID with the AssetManager here too.
    // EditorProjectManager::OpenProject does it after importing, but only when
    // the project opens; hot reloading the package runs this handler again
    // without OpenProject, and AssetRef::Get() would not find the cache path
    // until the editor restarts.
    Deki::AssetManager::Get()->RegisterGuid(ctx.guid, ctx.guid);

    DEKI_LOG_EDITOR("TilesetSync: baked '%s' -> %s (atlas=%s)", ctx.absolutePath.c_str(), ctx.guid.c_str(),
                    atlasGuid.c_str());
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

    // Each external tileset's GUID, from its .tsj. No recursive sync here:
    // the AssetPipeline runs the .tsj handlers itself.
    fs::path tmjPath = ctx.absolutePath;
    std::vector<BakedTilesetRef> baked;
    std::vector<DekiEditor::SubAssetInfo> subs;
    int subIdx = 0;
    for (const auto& tref : map.tilesets)
    {
        fs::path tsjAbs = (tmjPath.parent_path() / tref.source).lexically_normal();
        fs::path tsjRel = fs::relative(tsjAbs, ctx.projectPath);
        std::string tsjRelStr = tsjRel.generic_string();

        // Only JSON tilesets (.tsj) are supported, not .tsx (XML). Tiled
        // defaults to .tsx even for .tmj maps, so this is the most common
        // bake failure; the error gives the exact fix.
        std::string ext = tsjAbs.extension().string();
        for (char& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".tsx")
        {
            DEKI_LOG_ERROR("TilemapSync: '%s' references XML tileset '%s'. The deki-tilemap package is "
                           "JSON-only. In Tiled: open the .tsx, File > Export As > Tiled JSON Tileset (.tsj), "
                           "then update the map's tileset reference to the .tsj file. To stop hitting this: "
                           "Edit > Preferences > General > Store tilesets as > JSON.",
                           ctx.absolutePath.c_str(), tref.source.c_str());
            return DekiEditor::AssetCacheResult::NotCached;
        }

        std::string tsGuid = GuidForRelativePath(ctx.pipeline, tsjRelStr);
        if (tsGuid.empty())
        {
            DEKI_LOG_ERROR("TilemapSync: external tileset '%s' (referenced from '%s') has no GUID — "
                           "the .tsj file must live somewhere under the project's assets/ folder so the "
                           "editor can import it.",
                           tsjRelStr.c_str(), ctx.absolutePath.c_str());
            return DekiEditor::AssetCacheResult::NotCached;
        }

        baked.push_back({ tref.firstGid, tsGuid });

        // A sub-asset, so the asset browser shows the tileset under the map.
        DekiEditor::SubAssetInfo s;
        s.guid = tsGuid;
        s.parentGuid = ctx.guid;
        s.subAssetIndex = subIdx++;
        s.name = tsjAbs.stem().string();
        s.depth = 0;
        s.hasPreview = true;
        subs.push_back(s);
    }

    if (!WriteDtilemap(map, baked, ctx.cachePath))
    {
        DEKI_LOG_ERROR("TilemapSync: bake failed for '%s' (cache path '%s')", ctx.absolutePath.c_str(),
                       ctx.cachePath.c_str());
        return DekiEditor::AssetCacheResult::NotCached;
    }

    ctx.pipeline->RegisterSubAssets(ctx.guid, subs);

    // Register the GUID here too; see the same call in HandleTilesetCache.
    Deki::AssetManager::Get()->RegisterGuid(ctx.guid, ctx.guid);

    // Record the tileset GUIDs in the .data sidecar, for tools and for
    // comparing on hot reload.
    fs::path dataPath = ctx.absolutePath + std::string(".data");
    json sidecar;
    if (fs::exists(dataPath))
    {
        std::ifstream in(dataPath);
        try
        {
            sidecar = json::parse(in);
        }
        catch (...)
        {
            sidecar = json{};
        }
    }
    sidecar["baked"] = true;
    json& tilesets = sidecar["tilesets"];
    tilesets = json::object();
    for (const auto& b : baked)
    {
        tilesets[std::to_string(b.firstGid)] = json{ { "guid", b.guid } };
    }
    {
        std::ofstream out(dataPath);
        out << sidecar.dump(2);
    }

    DEKI_LOG_EDITOR("TilemapSync: baked '%s' -> %s (%zu tilesets, %zu layers)", ctx.absolutePath.c_str(),
                    ctx.guid.c_str(), baked.size(), map.tileLayers.size());
    return DekiEditor::AssetCacheResult::Cached;
}

}  // namespace

void RegisterTilemapSyncHandlers()
{
    if (s_SyncHandlerRegistered)
    {
        return;
    }
    s_SyncHandlerRegistered = true;

    DekiEditor::AssetPipeline::OnStarted(
        [](DekiEditor::AssetPipeline* p)
        {
            // Cache handlers, not sync handlers: returning AssetCacheResult::Cached
            // sets info.hasCachedVersion, so EditorProjectManager's post-import
            // loop registers the GUID that AssetRef::Get() needs at runtime. Sync
            // handlers run after hasCachedVersion is final.
            p->RegisterCacheHandler(".tsj", HandleTilesetCache);
            p->RegisterCacheHandler(".tmj", HandleTilemapCache);
        });
}

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
