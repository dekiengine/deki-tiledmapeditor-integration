/**
 * @file DekiTilemapPackage.cpp
 * @brief Package entry point for deki-tiledmap DLL
 */

#include "DekiTilemapPackage.h"
#include "interop/DekiPlugin.h"
#include "DekiPackageFeatureMeta.h"
#include "TilemapComponent.h"
#include "TilemapColliderComponent.h"
#include "TilemapObjectSpawner.h"
#include "TilemapRenderSystem.h"
#include "reflection/ComponentRegistry.h"
#include "reflection/ComponentFactory.h"

#ifdef DEKI_EDITOR
#include "editor/TilemapSyncHandler.h"
#include "editor/TilemapInspector.h"
#endif

#ifdef DEKI_EDITOR

#ifndef DEKI_PLUGIN_EXPORTS
extern void DekiTilemap_RegisterComponents();
extern int  DekiTilemap_GetAutoComponentCount();
extern const DekiComponentMeta* DekiTilemap_GetAutoComponentMeta(int index);

static bool s_Registered = false;
#endif

extern "C" {

#ifndef DEKI_PLUGIN_EXPORTS
DEKI_TILEDMAP_API int DekiTilemap_EnsureRegistered(void)
{
    if (s_Registered)
        return DekiTilemap_GetAutoComponentCount();
    s_Registered = true;

    DekiTilemap_RegisterComponents();

    DekiTilemap::RegisterTilemapSyncHandlers();
    DekiTilemap::RegisterTilemapInspector();

    return DekiTilemap_GetAutoComponentCount();
}
#endif // DEKI_PLUGIN_EXPORTS

} // extern "C"

extern "C" {

#ifndef DEKI_PLUGIN_EXPORTS
DEKI_PLUGIN_API const char* DekiPlugin_GetName(void)
{
    return "Deki Tiled Map Package";
}

DEKI_PLUGIN_API const char* DekiPlugin_GetVersion(void)
{
#ifdef DEKI_PACKAGE_VERSION
    return DEKI_PACKAGE_VERSION;
#else
    return "0.0.0-dev";
#endif
}

DEKI_PLUGIN_API const char* DekiPlugin_GetReflectionJson(void)
{
    return "{}";
}

DEKI_PLUGIN_API int DekiPlugin_Init(void)
{
    return 0;
}

DEKI_PLUGIN_API void DekiPlugin_Shutdown(void)
{
    s_Registered = false;
}

DEKI_PLUGIN_API int DekiPlugin_GetComponentCount(void)
{
    return DekiTilemap_GetAutoComponentCount();
}

DEKI_PLUGIN_API const DekiComponentMeta* DekiPlugin_GetComponentMeta(int index)
{
    return DekiTilemap_GetAutoComponentMeta(index);
}

DEKI_PLUGIN_API void DekiPlugin_RegisterComponents(void)
{
    DekiTilemap_EnsureRegistered();
}
#endif // DEKI_PLUGIN_EXPORTS

// =============================================================================
// Package Feature API
// =============================================================================

static const char* s_RenderGuids[]    = { TilemapComponent::StaticGuid };
static const char* s_ColliderGuids[]  = { TilemapColliderComponent::StaticGuid };
static const char* s_ObjectGuids[]    = { TilemapObjectSpawner::StaticGuid };

static const DekiPackageFeatureInfo s_Features[] = {
    {"tilemap_render",    "Tilemap Rendering", "Render Tiled maps via batched quad blits",
        true, "DEKI_FEATURE_TILEMAP_RENDER",    s_RenderGuids,   1},
    {"tilemap_collision", "Tilemap Collision", "Per-tile collision shapes from tileset objectgroups",
        true, "DEKI_FEATURE_TILEMAP_COLLISION", s_ColliderGuids, 1},
    {"tilemap_objects",   "Object Layers",     "Spawn engine scenes from Tiled object layers",
        true, "DEKI_FEATURE_TILEMAP_OBJECTS",   s_ObjectGuids,   1},
};

#ifndef DEKI_PLUGIN_EXPORTS
DEKI_PLUGIN_API int DekiPlugin_GetFeatureCount(void)
{
    return sizeof(s_Features) / sizeof(s_Features[0]);
}

DEKI_PLUGIN_API const DekiPackageFeatureInfo* DekiPlugin_GetFeature(int index)
{
    if (index < 0 || index >= DekiPlugin_GetFeatureCount())
        return nullptr;
    return &s_Features[index];
}
#endif // DEKI_PLUGIN_EXPORTS

// Package-specific feature API (linked-DLL access without name conflicts)

DEKI_TILEDMAP_API const char* DekiTilemap_GetName(void)
{
    return "Tiled Map";
}

DEKI_TILEDMAP_API int DekiTilemap_GetFeatureCount(void)
{
    return static_cast<int>(sizeof(s_Features) / sizeof(s_Features[0]));
}

DEKI_TILEDMAP_API const DekiPackageFeatureInfo* DekiTilemap_GetFeature(int index)
{
    if (index < 0 || index >= DekiTilemap_GetFeatureCount())
        return nullptr;
    return &s_Features[index];
}

} // extern "C"

#else // !DEKI_EDITOR — runtime-only build

// On non-editor targets, components register themselves via static
// initializers and there is no plugin export surface to expose.

#endif // DEKI_EDITOR
