/**
 * @file TilemapAssetTypes.cpp
 * @brief AssetTypeRegistry bindings for .tmj (Tilemap) and .tsj (Tileset).
 *
 * The actual baking is handled by TilemapSyncHandler via the asset-pipeline
 * sync-handler callback. These AssetTypeEditor subclasses only exist to map
 * extensions to type names so AssetPipeline::GetAssetTypeFromExtension and
 * IsAssetFile can resolve them through AssetTypeRegistry — same path every
 * other package uses.
 */

#ifdef DEKI_EDITOR

#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/AssetTypeRegistry.h>

namespace DekiEditor
{

class TilemapAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override    { return "Tilemap"; }
    const char* GetDisplayName() const override { return "Tilemap"; }
    std::vector<std::string> GetExtensions() const override { return { ".tmj" }; }
};

class TilesetAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override    { return "Tileset"; }
    const char* GetDisplayName() const override { return "Tileset"; }
    std::vector<std::string> GetExtensions() const override { return { ".tsj" }; }
};

REGISTER_EDITOR(TilemapAssetType)
REGISTER_EDITOR(TilesetAssetType)

namespace {
struct TilemapCategoryRegistrar
{
    TilemapCategoryRegistrar()
    {
        auto& reg = AssetTypeRegistry::Instance();
        reg.RegisterCategory(".tmj", AssetCategory::Data);
        reg.RegisterCategory(".tsj", AssetCategory::Data);
    }
};
static TilemapCategoryRegistrar s_TilemapCategoryRegistrar;
}

} // namespace DekiEditor

#endif // DEKI_EDITOR
