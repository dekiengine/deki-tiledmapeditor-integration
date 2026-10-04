#include "TilemapComponent.h"
#include "TilemapStreamer.h"  // SetMemoryBudget on the resolved map

namespace DekiTiledMap
{

TilemapComponent::TilemapComponent()
{
    tintColor = { 255, 255, 255, 255 };
}

bool TilemapComponent::RenderContent(const Deki::Object* /*owner*/, QuadBlit::Source& /*outSource*/,
                                     float& /*outPivotX*/, float& /*outPivotY*/, uint8_t& outTintR, uint8_t& outTintG,
                                     uint8_t& outTintB, uint8_t& outTintA)
{
    // TilemapRenderSystem draws the visible chunks per layer. Returning false
    // stops the standard renderer from drawing a single quad.
    outTintR = outTintG = outTintB = outTintA = 255;
    return false;
}

void TilemapComponent::OnAssetRefResolved(const char* /*propertyName*/, void* /*asset*/, const char* /*guid*/)
{
    // AssetRef tracks the Tilemap pointer and TilemapRenderSystem reads it each
    // frame, so only the chunk budget needs setting here, on the map's
    // streamer.
    //
    // The budget belongs to the map, which the asset manager shares between
    // every component using it, while this setting is per component. So it is
    // only raised, never lowered: two objects drawing one map get the larger
    // request, and neither starves the other.
    if (auto* map = static_cast<Tilemap*>(tilemap.ptr))
    {
        if (TilemapStreamer* streamer = map->Streamer())
        {
            const int32_t kib = chunkCacheKiB > 0 ? chunkCacheKiB : 1;
            const size_t wanted = static_cast<size_t>(kib) * 1024u;
            if (wanted > streamer->MemoryBudget())
            {
                streamer->SetMemoryBudget(wanted);
            }
        }
    }
}

void TilemapComponent::UnloadAssets()
{
    tilemap.ptr = nullptr;
    tilemap.loadAttempted = false;
}

}  // namespace DekiTiledMap
