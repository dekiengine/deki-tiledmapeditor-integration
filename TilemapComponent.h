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

    // Bitmask of layers to draw.
    DEKI_EXPORT
    DEKI_TOOLTIP("Which of the map's layers to draw, one bit per layer. All bits set draws everything.")
    int32_t visibleLayerMask = 0x7FFFFFFF;

    // Chunks loaded past each edge of the view.
    DEKI_EXPORT
    DEKI_TOOLTIP("How many chunks beyond the screen edge to keep loaded and drawn. A little padding stops tiles "
                 "popping in at the edge while scrolling.")
    int32_t chunkPadding = 1;

    // How much decoded chunk data the map may hold, set on the map's streamer
    // when the asset resolves. 256 KiB suits an ESP32; desktops can use more.
    //
    // KiB is in the name because there is no memory PhysicalUnit to carry it.
    DEKI_EXPORT
    DEKI_TOOLTIP("How much memory this map may keep decoded chunks in, in KiB. Past the budget the "
                 "least recently drawn chunks are dropped and re-read from storage when they are "
                 "needed again, so too small shows up as stutter while scrolling rather than as "
                 "anything missing. 256 suits an ESP32; a desktop can afford far more.")
    DEKI_RANGE(16.0f, 262144.0f)
    int32_t chunkCacheKiB = 256;

    // Tint for every drawn tile; white is none.
    DEKI_EXPORT
    DEKI_TOOLTIP("Multiplied into every tile. White leaves the map alone.")
    Deki::Color tintColor;

    // Map pixels per world meter: each tile pixel is 1/pixelsPerMeter meters.
    // When it equals the camera's and the project's pixels per meter, tiles
    // draw 1:1. The default 16 matches the project default.
    DEKI_EXPORT
    DEKI_TOOLTIP(
        "How many of the map's pixels make one meter. This is what lines the map up with everything else in the scene.")
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    // Loop the map on each axis. wrapPeriodX/Y set the repeat size: 0 uses
    // the map's bounds, more is a tile count.
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

    /// Returns false: TilemapRenderSystem draws the chunks itself, not as one quad.
    bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB, uint8_t& outTintA) override;

    void OnAssetRefResolved(const char* propertyName, void* asset, const char* guid) override;
    void UnloadAssets() override;
};

}  // namespace DekiTiledMap
