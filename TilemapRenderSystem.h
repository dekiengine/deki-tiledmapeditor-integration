#pragma once

#include "deki-rendering/RenderPass.h"

namespace DekiTilemap
{

// Per-object render pass that draws every TilemapComponent it sees.
//
// For each visible chunk on each enabled layer, emits one BlitScaled per
// non-zero tile against the tileset atlas. Drives the chunk streamer with
// the camera's visible rect (plus chunk_padding) and a per-frame IO budget.
//
// Registered with DekiRenderPassRegistry under the name "tilemap" so the
// project's .rpipeline can activate it.
class TilemapRenderPass : public RenderPass
{
public:
    static constexpr const char* RegistryName = "tilemap";

    void Execute(DekiObject* obj, RenderContext& ctx) override;
};

} // namespace DekiTilemap
