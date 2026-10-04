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
    int32_t x, y;
    int32_t width, height;
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

struct TmjMap
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t tileWidth = 0;
    int32_t tileHeight = 0;
    int32_t chunkWidth = 16;
    int32_t chunkHeight = 16;
    bool infinite = false;
    uint32_t backgroundColor = 0xFF000000;  // RGBA8, R in the low byte (ParseTiledColor)
    std::vector<TmjTilesetRef> tilesets;
    std::vector<TmjLayer> tileLayers;
    std::vector<TmjObjectLayer> objectLayers;
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
