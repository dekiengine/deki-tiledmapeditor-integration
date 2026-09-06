#pragma once

#ifdef DEKI_EDITOR

#include <cstdint>
#include <string>
#include <vector>

namespace DekiTilemap
{

struct TmjTilesetRef
{
    uint32_t    firstGid;
    std::string source;        // Relative path to .tsj (external only — embedded rejected)
};

enum class TmjEncoding : uint8_t
{
    CSV,
    Base64,
    Base64Zlib,
};

struct TmjChunk
{
    int32_t  x, y;             // chunk origin in tiles
    int32_t  width, height;    // chunk size in tiles
    std::vector<uint32_t> data;
};

struct TmjLayer
{
    std::string name;
    int32_t     id;
    bool        visible = true;
    // For finite layers: data[0..width*height). For infinite: chunks[].
    std::vector<uint32_t> data;
    std::vector<TmjChunk> chunks;
    int32_t               width = 0;
    int32_t               height = 0;
};

struct TmjPropertyValue
{
    std::string name;
    std::string type;          // "string"|"int"|"float"|"bool"
    std::string svalue;
    int32_t     ivalue = 0;
    float       fvalue = 0.0f;
    bool        bvalue = false;
};

struct TmjObject
{
    uint32_t    id;
    std::string name;
    std::string type;          // Tiled "class"
    int32_t     x, y;
    int32_t     width, height;
    float       rotation;
    uint32_t    gid = 0;
    bool        ellipse = false;
    std::vector<int32_t> polygonPoints;   // pairs of (x,y)
    std::vector<int32_t> polylinePoints;
    std::vector<TmjPropertyValue> properties;
};

struct TmjObjectLayer
{
    std::string name;
    int32_t     id;
    bool        visible = true;
    std::vector<TmjObject> objects;
};

struct TmjMap
{
    int32_t   width  = 0;
    int32_t   height = 0;
    int32_t   tileWidth  = 0;
    int32_t   tileHeight = 0;
    int32_t   chunkWidth  = 16;
    int32_t   chunkHeight = 16;
    bool      infinite    = false;
    uint32_t  backgroundColor = 0xFF000000;   // ABGR? we store RGBA below
    std::vector<TmjTilesetRef> tilesets;
    std::vector<TmjLayer>      tileLayers;
    std::vector<TmjObjectLayer> objectLayers;
};

// Tileset (.tsj) IR
struct TmjTilesetTile
{
    uint32_t id;
    std::vector<TmjPropertyValue> properties;
    // Animation
    struct Frame { uint32_t tileId; uint32_t durationMs; };
    std::vector<Frame> animation;
    // Collision: only the first object is honored in v1.
    bool     hasCollision = false;
    uint32_t collisionShape = 0;   // matches DTileCollisionShape
    int32_t  cx = 0, cy = 0;
    int32_t  cw = 0, ch = 0;
    std::vector<int32_t> collisionPolygon;
};

struct TmjTileset
{
    std::string name;
    int32_t     tileWidth = 0;
    int32_t     tileHeight = 0;
    int32_t     tileCount = 0;
    int32_t     columns = 0;
    int32_t     rows = 0;
    std::string imageRelative;       // path to PNG, relative to .tsj
    std::vector<TmjTilesetTile> tiles;

    // Tiled "transparentcolor" property: pixels of this exact RGB are skipped
    // by the renderer (chroma key). Stored RGBA-packed (R in low byte) per
    // ParseTiledColor convention; the alpha byte is unused for keying.
    bool        hasTransparentColor = false;
    uint32_t    transparentColor = 0;
};

// Parse a .tmj file into TmjMap. Returns false and sets outError on failure.
// Embedded tilesets, gzip/zstd compression, and base64+gzip layers are
// rejected loudly per project policy.
bool ParseTmjMap(const std::string& tmjAbsPath, TmjMap& outMap, std::string& outError);

// Parse a .tsj file into TmjTileset.
bool ParseTsjTileset(const std::string& tsjAbsPath, TmjTileset& outTs, std::string& outError);

} // namespace DekiTilemap

#endif // DEKI_EDITOR
