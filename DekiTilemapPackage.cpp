/**
 * @file DekiTilemapPackage.cpp
 * @brief Package entry point for deki-tiledmap DLL
 */

#include "DekiTilemapPackage.h"
#include <deki/interop/Plugin.h>
#include "TilemapComponent.h"
#include "TilemapColliderComponent.h"
#include "TilemapObjectSpawner.h"
#include "TilemapRenderSystem.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>

#ifdef DEKI_EDITOR
#include "editor/TilemapSyncHandler.h"
#include "editor/TilemapInspector.h"
#endif

extern void DekiTilemapRegisterComponents();
extern int DekiTilemapGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiTilemapGetAutoComponentMeta(int index);

namespace DekiTiledMap
{

#ifdef DEKI_EDITOR

#ifndef DEKI_PLUGIN_EXPORTS

static bool s_Registered = false;
#endif

// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiTiledMap;

extern "C"
{
#ifndef DEKI_PLUGIN_EXPORTS
    DEKI_TILEDMAP_API int DekiTilemapEnsureRegistered(void)
    {
        if (s_Registered)
        {
            return ::DekiTilemapGetAutoComponentCount();
        }
        s_Registered = true;

        ::DekiTilemapRegisterComponents();

        DekiTiledMap::RegisterTilemapSyncHandlers();
        DekiTiledMap::RegisterTilemapInspector();

        return ::DekiTilemapGetAutoComponentCount();
    }
#endif  // DEKI_PLUGIN_EXPORTS

}  // extern "C"

extern "C"
{
#ifndef DEKI_PLUGIN_EXPORTS
    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Tiled Map Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        s_Registered = false;
    }

    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiTilemapGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiTilemapGetAutoComponentMeta(index);
    }

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        DekiTilemapEnsureRegistered();
    }
#endif  // DEKI_PLUGIN_EXPORTS

    // Package-specific feature API (linked-DLL access without name conflicts)

    DEKI_TILEDMAP_API const char* DekiTilemapGetName(void)
    {
        return "Tiled Map";
    }

}  // extern "C"

#else  // !DEKI_EDITOR — runtime-only build

// On non-editor targets, components register themselves via static
// initializers and there is no plugin export surface to expose.

#endif  // DEKI_EDITOR
}  // namespace DekiTiledMap
