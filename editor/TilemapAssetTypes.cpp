// AssetTypeRegistry entries for .tmj (Tilemap) and .tsj (Tileset).
//
// TilemapSyncHandler does the baking, through the asset pipeline's cache
// handlers. These AssetTypeEditor subclasses only map the extensions to type
// names, so AssetPipeline::GetAssetTypeFromExtension and IsAssetFile find them
// through AssetTypeRegistry, as for every other package.

#ifdef DEKI_EDITOR

#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/AssetTypeRegistry.h>

namespace DekiEditor
{

class TilemapAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "Tilemap"; }
    const char* GetDisplayName() const override { return "Tilemap"; }
    std::vector<std::string> GetExtensions() const override { return { ".tmj" }; }
};

class TilesetAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "Tileset"; }
    const char* GetDisplayName() const override { return "Tileset"; }
    std::vector<std::string> GetExtensions() const override { return { ".tsj" }; }
};

REGISTER_EDITOR(TilemapAssetType)
REGISTER_EDITOR(TilesetAssetType)

namespace
{
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
}  // namespace

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
