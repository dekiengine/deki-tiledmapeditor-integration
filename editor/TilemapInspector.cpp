#ifdef DEKI_EDITOR

#include "TilemapInspector.h"

#include <string>

#include "DekiLogSystem.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiTilemap
{

namespace
{

bool s_InspectorRegistered = false;

void OpenInTiled(const std::string& absPath)
{
#ifdef _WIN32
    HINSTANCE rc = ShellExecuteA(nullptr, "open", absPath.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
    {
        DEKI_LOG_ERROR("TilemapInspector: failed to open '%s' in Tiled. Make sure Tiled is "
                       "installed and registered as the .tmj handler.", absPath.c_str());
    }
#else
    std::string cmd = "xdg-open '" + absPath + "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0)
        DEKI_LOG_ERROR("TilemapInspector: xdg-open failed for '%s'", absPath.c_str());
#endif
}

} // namespace

void RegisterTilemapInspector()
{
    if (s_InspectorRegistered) return;
    s_InspectorRegistered = true;

    // The editor's FileInspector registry isn't exposed via a stable C++ header
    // for package use yet. v1 ships the OpenInTiled helper as a public symbol
    // that the project's component custom-editor can call from its inspector
    // panel. Future revisions will subscribe directly once the registry API
    // is available.
    DEKI_LOG_EDITOR("TilemapInspector: ready (OpenInTiled available to component editors)");
    (void)&OpenInTiled;     // silence unused-function warning
}

} // namespace DekiTilemap

#endif // DEKI_EDITOR
