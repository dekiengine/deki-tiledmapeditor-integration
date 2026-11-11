#pragma once

#ifdef DEKI_EDITOR

#include <cstdint>
#include <string>
#include <vector>

namespace DekiTiledMap
{

struct TmjTilesetRef
{
    uint32_t firstGid;
    std::string source;  // relative path to the .tsj (embedded tilesets are rejected)
};

enum class TmjEncoding : uint8_t
{
    CSV,
    Base64,
    Base64Zlib,
};

struct TmjChunk
{
    int32_t x, y;           // chunk origin in tiles
    int32_t width, height;  // chunk size in tiles
    std::vector<uint32_t> data;
};

struct TmjLayer
{
    std::string name;
    int32_t id;
    bool visible = true;
    // Finite layers fill data[0..width*height); infinite ones fill chunks.
    std::vector<uint32_t> data;
    std::vector<TmjChunk> chunks;
    int32_t width = 0;
    int32_t height = 0;
};

struct TmjPropertyValue
{
    std::string name;
    std::string type;  // "string"|"int"|"float"|"bool"
    std::string svalue;
    int32_t ivalue = 0;
    float fvalue = 0.0f;
    bool bvalue = false;
};

struct TmjObject
{
    uint32_t id;
    std::string name;
    std::string type;  // Tiled "class"
    float x, y;  // pixels, as Tiled keeps them
    float width, height;
    float rotation;
    uint32_t gid = 0;
    bool ellipse = false;
    std::vector<int32_t> polygonPoints;  // pairs of (x,y)
    std::vector<int32_t> polylinePoints;
    std::vector<TmjPropertyValue> properties;
};

struct TmjObjectLayer
{
    std::string name;
    int32_t id;
    bool visible = true;
    std::vector<TmjObject> objects;
};

// How Tiled draws one layer, with its groups' settings folded in. TmjMap's
// layers lists every tile, object and image layer in drawing order.
struct TmjLayerLook
{
    enum class Kind
    {
        Tile,
        Object,
        Image,
    };
    Kind kind = Kind::Tile;
    uint32_t index = 0;  // into tileLayers, objectLayers or the image layers
    bool visible = true;
    float opacity = 1.0f;
    uint32_t tint = 0xFFFFFFFFu;  // RGBA8, R in the low byte
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float parallaxX = 1.0f;
    float parallaxY = 1.0f;
    uint8_t blendMode = 0;  // DBlendMode
    std::string imageRelative;  // image layers: the image, relative to the .tmj
    bool repeatX = false;
    bool repeatY = false;
};

struct TmjMap
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t tileWidth = 0;
    int32_t tileHeight = 0;
    int32_t chunkWidth = 16;
    int32_t chunkHeight = 16;
    bool infinite = false;
    bool isometric = false;
    // Staggered and hexagonal maps: which rows (or columns) are shifted.
    bool staggered = false;
    bool hexagonal = false;
    bool staggerX = false;     // Stagger Axis X: columns, not rows
    bool staggerEven = false;  // Stagger Index Even: the even ones, not the odd
    int32_t hexSideLength = 0;
    uint32_t backgroundColor = 0xFF000000;  // RGBA8, R in the low byte (ParseTiledColor)
    std::vector<TmjTilesetRef> tilesets;
    std::vector<TmjLayer> tileLayers;
    std::vector<TmjObjectLayer> objectLayers;
    std::vector<TmjLayerLook> layers;  // every layer, bottom first
    uint32_t imageLayerCount = 0;
    float parallaxOriginX = 0.0f;
    float parallaxOriginY = 0.0f;
};

// A parsed tileset (.tsj).
struct TmjTilesetTile
{
    uint32_t id;
    std::vector<TmjPropertyValue> properties;
    // Animation
    struct Frame
    {
        uint32_t tileId;
        uint32_t durationMs;
    };
    std::vector<Frame> animation;
    // Collision: only the tile's first collision object is used.
    bool hasCollision = false;
    uint32_t collisionShape = 0;  // matches DTileCollisionShape
    int32_t cx = 0, cy = 0;
    int32_t cw = 0, ch = 0;
    std::vector<int32_t> collisionPolygon;
};

struct TmjTileset
{
    std::string name;
    int32_t margin = 0;
    int32_t spacing = 0;
    int32_t tileOffsetX = 0;
    int32_t tileOffsetY = 0;
    uint32_t objectAlignment = 0;  // TileObjectAlignment
    bool renderToGrid = false;     // Tile Render Size: Map Grid Size
    bool preserveAspect = false;   // Fill Mode: Preserve Aspect Fit
    int32_t tileWidth = 0;
    int32_t tileHeight = 0;
    int32_t tileCount = 0;
    int32_t columns = 0;
    int32_t rows = 0;
    std::string imageRelative;  // path to PNG, relative to .tsj
    std::vector<TmjTilesetTile> tiles;

    // Tiled "transparentcolor": the renderer skips pixels of exactly this RGB
    // (chroma key). Packed as ParseTiledColor does, R in the low byte; the
    // alpha byte is not used.
    bool hasTransparentColor = false;
    uint32_t transparentColor = 0;
};

/// Parses a .tmj file. Returns false and sets outError on failure. Embedded
/// tilesets and compressed layers are rejected with an error.
bool ParseTmjMap(const std::string& tmjAbsPath, TmjMap& outMap, std::string& outError);

/// Parses a .tsj file. Returns false and sets outError on failure.
bool ParseTsjTileset(const std::string& tsjAbsPath, TmjTileset& outTs, std::string& outError);

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
