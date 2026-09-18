#include "TilemapComponent.h"
#include "TilemapStreamer.h"  // SetMemoryBudget on the resolved map

namespace DekiTiledMap
{

TilemapComponent::TilemapComponent()
{
    tintColor = {255, 255, 255, 255};
}

bool TilemapComponent::RenderContent(const Deki::Object* /*owner*/,
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
    // The Tilemap pointer itself is tracked by AssetRef, and TilemapRenderSystem
    // reads the live pointer each frame, so there is nothing to wire up for the
    // map. The chunk budget is the exception: it lives on the map's streamer and
    // nothing set it, so every target ran on the 256 KiB default.
    //
    // The budget belongs to the MAP, which the asset manager shares between
    // every component referencing it, while this setting is per component. So
    // raise only, never lower: two objects drawing one map settle on the larger
    // request instead of the last one resolved, and neither can starve the
    // other's view of it.
    if (auto* map = static_cast<Tilemap*>(tilemap.ptr))
    {
        if (TilemapStreamer* streamer = map->Streamer())
        {
            const int32_t kib = chunkCacheKiB > 0 ? chunkCacheKiB : 1;
            const size_t wanted = static_cast<size_t>(kib) * 1024u;
            if (wanted > streamer->MemoryBudget())
                streamer->SetMemoryBudget(wanted);
        }
    }
}

void TilemapComponent::UnloadAssets()
{
    tilemap.ptr = nullptr;
    tilemap.loadAttempted = false;
}

}  // namespace DekiTiledMap
