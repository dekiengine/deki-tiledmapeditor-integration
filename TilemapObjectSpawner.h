#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <deki/Component.h>
#include "Tilemap.h"
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

namespace Deki
{
class ICamera;
class Object;
class Scene;
}
namespace Deki2D
{
class SpriteComponent;
}

namespace DekiTiledMap
{

// On Awake, spawns an object for each object in the tilemap's object layers,
// into an object named "<owner> objects" beside the owner, so they sort with
// the rest of the scene:
//
//   - If the Tiled object has a "scene_guid" string property, that scene is
//     instantiated at the object's transform.
//   - A tile object becomes an object with a sprite of that tile, standing
//     where Tiled draws it.
//   - Anything else becomes an empty Deki::Object with the Tiled object's
//     name and transform.
//
// The objects of a layer with parallax go in an object named after the layer,
// inside the holder, which is moved with the view before each frame as Tiled
// moves the layer.
DEKI_CATEGORY("Tilemap")
DEKI_DESCRIPTION("Spawns objects from a Tiled map's object layers when the scene loads.")
class TilemapObjectSpawner : public Deki::Component
{
public:
    DEKI_EXPORT
    DEKI_TOOLTIP("The map whose object layer is read. Each object placed in Tiled becomes an object in the scene.")
    Deki::AssetRef<DekiTiledMap::Tilemap> tilemap;

    // Map pixels per world meter, to turn Tiled positions into meters. Should
    // match the TilemapComponent's pixelsPerMeter (default 16).
    DEKI_EXPORT
    DEKI_TOOLTIP("How many of the map's pixels make one meter, so spawned objects land where Tiled put them.")
    DEKI_RANGE(1.0f, 256.0f)
    float pixelsPerMeter = 16.0f;

    DEKI_EXPORT
    DEKI_TOOLTIP("The Sorting Order of a tile object standing at the world's horizon line (Y 0).")
    int32_t sortBase = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("How much a tile object's Sorting Order rises for each meter lower its point stands. Above 0, "
                 "what is lower on the screen draws on top, so characters can walk behind things. Set it to the "
                 "Pixels Per Meter for one order per pixel row, which stacks objects as the Tiled editor does. 0 "
                 "gives every tile object the Sort Base.")
    int32_t sortPerMeter = 0;

    TilemapObjectSpawner();
    ~TilemapObjectSpawner() override;

    void Awake() override;
    void Update() override;

    /// Moves every spawner's parallax layers for a frame seen through
    /// `camera`. Registered with Deki::ScreenAnchor::AddPlacer.
    static void PlaceParallaxLayers(const Deki::Scene* scene, const Deki::ICamera& camera, int screenWidth,
                                    int screenHeight);

private:
    int32_t BandOf(const Tilemap& tm, const DTilemapObject& obj) const;
    // A spawned object by id, if it is still there: looked for in the holder,
    // beside the owner, and two levels into it (objects, and those in a
    // parallax layer's object), not in the whole scene.
    Deki::Object* FindSpawned(uint32_t id) const;
    // The holder, if it is still there.
    Deki::Object* Holder() const;

    uint32_t m_HolderId = 0;
    void StackLayersAboveObjects(Tilemap& tm, Deki::Object* owner, Deki::Object* holder);

    std::vector<Deki::Object*> m_MSpawned;

    // A spawned tile object whose tile is animated in its tileset.
    struct AnimatedPiece
    {
        uint32_t objectId = 0;  // looked up when the frame changes: game code may destroy it
        std::vector<int32_t> rects;  // x, y, w, h of each frame in the atlas
        std::vector<uint32_t> endsMs;
        uint32_t totalMs = 0;
        int32_t shown = -1;
        int32_t due = -1;  // the frame to show, found this update
    };
    std::vector<AnimatedPiece> m_Animated;
    // Which of m_Animated an object id is: one walk over the holder updates
    // every piece whose frame changed.
    std::unordered_map<uint32_t, uint32_t> m_AnimatedById;

    // An object layer with parallax: the object holding its objects.
    struct ParallaxLayer
    {
        uint32_t objectId = 0;  // looked up each frame: game code may destroy it
        float factorX = 1.0f;
        float factorY = 1.0f;
    };
    std::vector<ParallaxLayer> m_Parallax;

    // Every live spawner, linked through the instances themselves.
    static TilemapObjectSpawner* s_First;
    TilemapObjectSpawner* m_Prev = nullptr;
    TilemapObjectSpawner* m_Next = nullptr;
};

}  // namespace DekiTiledMap
