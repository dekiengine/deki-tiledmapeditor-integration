#pragma once

#include <cstdint>

#include "deki-rendering/RendererComponent.h"
#include "Tilemap.h"
#include "Color.h"
#include "assets/AssetRef.h"
#include "reflection/DekiProperty.h"

class TilemapComponent : public RendererComponent
{
public:
    DEKI_COMPONENT(TilemapComponent, RendererComponent, "Tilemap",
                   "7c9a1d20-3e44-4b8a-9f12-6d2c5e3a8b71",
                   "DEKI_FEATURE_TILEMAP_RENDER")

    DEKI_EXPORT
    Deki::AssetRef<DekiTilemap::Tilemap> tilemap;

    // Bitmask of layers to draw. Default: all layers.
    DEKI_EXPORT
    int32_t visible_layer_mask = 0x7FFFFFFF;

    // Number of chunks loaded past the visible viewport edge (per side).
    DEKI_EXPORT
    int32_t chunk_padding = 1;

    // Tint applied to every drawn tile. White = no tint.
    DEKI_EXPORT
    deki::Color tint_color;

    TilemapComponent();

    // Returns false: TilemapRenderSystem renders chunks itself, not as a single quad.
    bool RenderContent(const DekiObject* owner,
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

#include "generated/TilemapComponent.gen.h"
