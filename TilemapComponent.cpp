#include "TilemapComponent.h"

TilemapComponent::TilemapComponent()
{
    tint_color = {255, 255, 255, 255};
}

bool TilemapComponent::RenderContent(const DekiObject* /*owner*/,
                                     QuadBlit::Source& /*outSource*/,
                                     float& /*outPivotX*/,
                                     float& /*outPivotY*/,
                                     uint8_t& outTintR,
                                     uint8_t& outTintG,
                                     uint8_t& outTintB,
                                     uint8_t& outTintA)
{
    // Tilemap drawing happens in TilemapRenderSystem so we can iterate visible
    // chunks per layer. Returning false here prevents the standard renderer
    // from trying to blit a single quad for the component.
    outTintR = outTintG = outTintB = outTintA = 255;
    return false;
}

void TilemapComponent::OnAssetRefResolved(const char* /*propertyName*/,
                                          void* /*asset*/,
                                          const char* /*guid*/)
{
    // Tilemap pointer is tracked by AssetRef directly. Nothing to wire up here
    // — TilemapRenderSystem reads the live pointer each frame.
}

void TilemapComponent::UnloadAssets()
{
    tilemap.ptr = nullptr;
    tilemap.loadAttempted = false;
}
