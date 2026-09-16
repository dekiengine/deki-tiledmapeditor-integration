#pragma once

#include <cstdint>

#include "deki-rendering/RendererComponent.h"
#include "Tilemap.h"
#include <deki/Color.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace DekiTiledMap
{

DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Draws a Tiled map, streaming chunks in around the camera.")
DEKI_FORMER_NAME("TilemapComponent")
class TilemapComponent : public DekiRendering::RendererComponent
{
public:

    DEKI_EXPORT
    DEKI_TOOLTIP("A map exported from Tiled.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    // Bitmask of layers to draw. Default: all layers.
    DEKI_EXPORT
    DEKI_TOOLTIP("Which of the map's layers to draw, one bit per layer. All bits set draws everything.")
    int32_t visibleLayerMask = 0x7FFFFFFF;

    // Number of chunks loaded past the visible viewport edge (per side).
    DEKI_EXPORT
    DEKI_TOOLTIP("How much beyond the screen edge to keep drawn, in tiles. A little padding stops tiles popping in at the edge while scrolling.")
    int32_t chunkPadding = 1;

    // Tint applied to every drawn tile. White = no tint.
    DEKI_EXPORT
    DEKI_TOOLTIP("Multiplied into every tile. White leaves the map alone.")
    Deki::Color tintColor;

    // Source pixels per world meter for this tilemap. The renderer treats
    // each tile pixel as 1/pixelsPerMeter meters of world space. When
    // pixelsPerMeter equals camera.pixelsPerMeter and project PPM, tiles
    // render 1:1 with their source. Default 16 matches the project default.
    DEKI_EXPORT
    DEKI_TOOLTIP("How many of the map's pixels make one meter. This is what lines the map up with everything else in the scene.")
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    // Loop the map on each axis. When enabled, wrap_period controls the
    // strip size: 0 = auto (use authored bounds), >0 = explicit tile count.
    DEKI_EXPORT
    DEKI_TOOLTIP("Repeat the map horizontally, so scrolling past the edge wraps round.")
    bool loopX = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("How wide one horizontal repeat is, in tiles. 0 uses the map's own width.")
    DEKI_VISIBLE_WHEN(loopX, 1)
    int32_t wrapPeriodX = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Repeat the map vertically.")
    bool loopY = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("How tall one vertical repeat is, in tiles. 0 uses the map's own height.")
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

}  // namespace DekiTiledMap

