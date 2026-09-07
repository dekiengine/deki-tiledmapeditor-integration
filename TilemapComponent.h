#pragma once

#include <cstdint>

#include "deki-rendering/RendererComponent.h"
#include "Tilemap.h"
#include <deki/Color.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Draws a Tiled map, streaming chunks in around the camera.")
class TilemapComponent : public RendererComponent
{
public:

    DEKI_EXPORT
    Deki::AssetRef<DekiTilemap::Tilemap> tilemap;

    // Bitmask of layers to draw. Default: all layers.
    DEKI_EXPORT
    int32_t visibleLayerMask = 0x7FFFFFFF;

    // Number of chunks loaded past the visible viewport edge (per side).
    DEKI_EXPORT
    int32_t chunkPadding = 1;

    // Tint applied to every drawn tile. White = no tint.
    DEKI_EXPORT
    Deki::Color tintColor;

    // Source pixels per world meter for this tilemap. The renderer treats
    // each tile pixel as 1/pixelsPerMeter meters of world space. When
    // pixelsPerMeter equals camera.pixelsPerMeter and project PPM, tiles
    // render 1:1 with their source. Default 16 matches the project default.
    DEKI_EXPORT
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    // Loop the map on each axis. When enabled, wrap_period controls the
    // strip size: 0 = auto (use authored bounds), >0 = explicit tile count.
    DEKI_EXPORT
    bool loopX = false;

    DEKI_EXPORT
    DEKI_VISIBLE_WHEN(loopX, 1)
    int32_t wrapPeriodX = 0;

    DEKI_EXPORT
    bool loopY = false;

    DEKI_EXPORT
    DEKI_VISIBLE_WHEN(loopY, 1)
    int32_t wrapPeriodY = 0;

    TilemapComponent();

    // Returns false: TilemapRenderSystem renders chunks itself, not as a single quad.
    bool RenderContent(const Deki::Object* owner,
                       QuadBlit::Source& outSource,
                       float& outPivotX,
                       float& outPivotY,
                       uint8_t& outTintR,
                       uint8_t& outTintG,
                       uint8_t& outTintB,
                       uint8_t& outTintA) override;

    void OnAssetRefResolved(const char* propertyName, void* asset, const char* guid) override;
    void UnloadAssets() override;
};

