#ifdef DEKI_EDITOR

#include "TilemapInspector.h"

#include <string>

#include <deki/LogSystem.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace DekiTiledMap
{

namespace
{

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

}  // namespace

void RegisterTilemapInspector()
{
    if (s_InspectorRegistered)
    {
        return;
    }
    s_InspectorRegistered = true;

    // The editor's FileInspector registry has no stable header for packages
    // yet, so nothing is registered here. OpenInTiled is a public helper a
    // project's custom editor can call from its inspector.
    (void)&OpenInTiled;  // no unused-function warning
}

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
