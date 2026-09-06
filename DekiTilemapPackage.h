#pragma once

#ifdef _WIN32
    #if defined(DEKI_TILEDMAP_EXPORTS) || defined(DEKI_PLUGIN_EXPORTS)
        #define DEKI_TILEDMAP_API __declspec(dllexport)
    #else
        #define DEKI_TILEDMAP_API __declspec(dllimport)
    #endif
#else
    #define DEKI_TILEDMAP_API __attribute__((visibility("default")))
#endif

#ifdef DEKI_PACKAGE_TILEDMAP

#ifndef DEKI_PACKAGE_FEATURES_CONFIGURED
#define DEKI_FEATURE_TILEMAP_RENDER
#define DEKI_FEATURE_TILEMAP_COLLISION
#define DEKI_FEATURE_TILEMAP_OBJECTS
#endif

#include "Tilemap.h"
#include "Tileset.h"

#if !defined(DEKI_EDITOR) || !defined(DEKI_ENGINE_EXPORTS)
#ifdef DEKI_FEATURE_TILEMAP_RENDER
#include "TilemapComponent.h"
#endif
#ifdef DEKI_FEATURE_TILEMAP_COLLISION
#include "TilemapColliderComponent.h"
#endif
#ifdef DEKI_FEATURE_TILEMAP_OBJECTS
#include "TilemapObjectSpawner.h"
#endif
#endif

#endif // DEKI_PACKAGE_TILEDMAP
