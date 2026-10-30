#ifdef DEKI_EDITOR

#include "TilemapInspector.h"

#include "TmjParser.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/Localization.h>
#include <deki/LogSystem.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiTiledMap
{

namespace
{

namespace fs = std::filesystem;

bool s_InspectorRegistered = false;

void OpenInTiled(const std::string& absPath)
{
#ifdef _WIN32
    HINSTANCE rc = ShellExecuteA(nullptr, "open", absPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
    {
        DEKI_LOG_ERROR("TilemapInspector: failed to open '%s' in Tiled. Make sure Tiled is "
                       "installed and registered as the .tmj handler.",
                       absPath.c_str());
    }
#else
    std::string cmd = "xdg-open '" + absPath + "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0)
    {
        DEKI_LOG_ERROR("TilemapInspector: xdg-open failed for '%s'", absPath.c_str());
    }
#endif
}

// What a Tiled map or tileset is, read from the file: Tiled edits it, the
// editor bakes it when it changes, so the inspector shows it and opens Tiled.
// The file is parsed again only when it changes on disk.
class TilemapFileInspector : public DekiEditor::FileInspector
{
public:
    const char** GetExtensions() const override
    {
        static const char* kExtensions[] = { ".tmj", ".tsj" };
        return kExtensions;
    }
    int GetExtensionCount() const override { return 2; }

    void OnInspectorGUI(const std::string& assetPath, const std::string&) override
    {
        auto& ui = DekiEditor::EditorUI::Get();
        Refresh(assetPath);
        if (!ui.SectionHeader(DekiEditor::Tr(m_IsMap ? "Tilemap" : "Tileset")))
        {
            return;
        }

        char buf[64];
        if (!m_Error.empty())
        {
            ui.Hint(m_Error.c_str());
        }
        else if (m_IsMap)
        {
            std::snprintf(buf, sizeof(buf), "%d x %d", m_Map.width, m_Map.height);
            ui.PropertyRow("Size in Tiles");
            ui.ValueText(m_Map.infinite ? DekiEditor::Tr("infinite") : buf);
            std::snprintf(buf, sizeof(buf), "%d x %d px", m_Map.tileWidth, m_Map.tileHeight);
            ui.PropertyRow("Tile Size");
            ui.ValueText(buf);
            std::snprintf(buf, sizeof(buf), "%zu", m_Map.tileLayers.size());
            ui.PropertyRow("Tile Layers");
            ui.ValueText(buf);
            std::snprintf(buf, sizeof(buf), "%zu", m_Map.objectLayers.size());
            ui.PropertyRow("Object Layers");
            ui.ValueText(buf);
            std::snprintf(buf, sizeof(buf), "%zu", m_Map.tilesets.size());
            ui.PropertyRow("Tilesets");
            ui.ValueText(buf);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%d x %d px", m_Tileset.tileWidth, m_Tileset.tileHeight);
            ui.PropertyRow("Tile Size");
            ui.ValueText(buf);
            std::snprintf(buf, sizeof(buf), "%d", m_Tileset.tileCount);
            ui.PropertyRow("Tiles");
            ui.ValueText(buf);
            std::snprintf(buf, sizeof(buf), "%d", m_Tileset.columns);
            ui.PropertyRow("Columns");
            ui.ValueText(buf);
            ui.PropertyRow("Image");
            ui.ValueText(fs::path(m_Tileset.imageRelative).filename().string().c_str());
        }

        ui.Hint(DekiEditor::Tr("Made in Tiled. Edit it there; the editor bakes it again when the file changes."));
        if (ui.ActionButton(DekiEditor::TrLabel("Open in Tiled")))
        {
            OpenInTiled(assetPath);
        }
    }

private:
    void Refresh(const std::string& assetPath)
    {
        std::error_code ec;
        const auto stamp = fs::last_write_time(assetPath, ec);
        if (assetPath == m_Path && !ec && stamp == m_Stamp)
        {
            return;
        }
        m_Path = assetPath;
        m_Stamp = stamp;
        m_Error.clear();
        m_IsMap = fs::path(assetPath).extension() == ".tmj";
        m_Map = TmjMap{};
        m_Tileset = TmjTileset{};
        if (m_IsMap)
        {
            ParseTmjMap(assetPath, m_Map, m_Error);
        }
        else
        {
            ParseTsjTileset(assetPath, m_Tileset, m_Error);
        }
    }

    std::string m_Path;
    fs::file_time_type m_Stamp{};
    bool m_IsMap = true;
    TmjMap m_Map;
    TmjTileset m_Tileset;
    std::string m_Error;
};

TilemapFileInspector s_TilemapFileInspector;

}  // namespace

void RegisterTilemapInspector()
{
    if (s_InspectorRegistered)
    {
        return;
    }
    s_InspectorRegistered = true;
    DekiEditor::FileInspectorRegistry::Instance().Register(&s_TilemapFileInspector);
}

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
