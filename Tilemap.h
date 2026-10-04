#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Tileset.h"
#include "TileChunk.h"
#include <deki/assets/AssetRef.h>

namespace DekiTiledMap
{

class TilemapStreamer;

#pragma pack(push, 1)
struct DTilemapHeader
{
    char magic[4];      // 'D','T','M','1'
    uint32_t version;   // 1
    uint32_t mapWidth;  // tiles, 0xFFFFFFFF = infinite
    uint32_t mapHeight;
    uint16_t tileWidth;
    uint16_t tileHeight;
    uint16_t chunkWidth;
    uint16_t chunkHeight;
    uint32_t layerCount;
    uint32_t chunkIndexOffset;
    uint32_t chunkIndexCount;
    uint32_t tilesetTableOffset;
    uint32_t tilesetCount;
    uint32_t objectLayerOffset;
    uint32_t objectLayerCount;
    uint32_t backgroundColor;  // RGBA8
    uint32_t flags;            // bit0 = infinite, bit1 = kTilemapHasPools
    // Where the per-object pools are; valid when flags has kTilemapHasPools.
    // Maps baked before that leave them zero and the loader finds the pools
    // from where the baker put them.
    uint32_t pointPoolOffset;      // int32_t[2 * pointPoolCount], (x, y) pairs
    uint32_t pointPoolCount;       // points
    uint32_t propertyTableOffset;  // DTilemapProperty[propertyCount]
    uint32_t propertyCount;
    uint32_t stringPoolOffset;  // runs to the end of the file
};
static_assert(sizeof(DTilemapHeader) == 80, "DTilemapHeader layout drift");

constexpr uint32_t kTilemapHasPools = 1u << 1;

struct ChunkIndexEntry
{
    int32_t chunkX;
    int32_t chunkY;
    uint16_t layerIndex;
    uint16_t flags;  // ChunkIndexFlags
    uint32_t payloadOffset;
};
static_assert(sizeof(ChunkIndexEntry) == 16, "ChunkIndexEntry must be 16 bytes");

struct TilesetRef
{
    char guid[37];
    uint8_t pad0[3];
    uint32_t firstGid;
};

// Object layer table entry. Objects themselves are stored in a flat blob
// referenced by objectOffset/objectCount.
struct DObjectLayer
{
    char name[32];          // null-terminated, truncated
    uint32_t objectOffset;  // file offset to DTilemapObject[]
    uint32_t objectCount;
    uint32_t flags;  // reserved
    uint32_t pad;
};

enum class DObjectShape : uint32_t
{
    Rect = 0,
    Ellipse = 1,
    Polygon = 2,
    Polyline = 3,
    Tile = 4,  // gid != 0
};

struct DTilemapObject
{
    uint32_t id;     // Tiled's object id
    uint32_t shape;  // DObjectShape
    uint32_t gid;    // 0 for non-tile objects
    int32_t x, y;    // pixels
    int32_t width, height;
    float rotation;        // degrees, Tiled convention (converted to radians at spawn time)
    uint32_t pointOffset;  // first point in PolygonPoints(), in points (polygons, polylines)
    uint32_t pointCount;
    uint32_t propertyOffset;  // first entry in Properties()
    uint32_t propertyCount;
    char name[32];
    char type[32];  // Tiled's object class
};
#pragma pack(pop)

// Property pool entry. Property names and string values live in a separate
// string pool; this entry stores their offsets and a tag.
enum class DPropertyType : uint32_t
{
    String = 0,
    Int = 1,
    Float = 2,
    Bool = 3,
};

#pragma pack(push, 1)
struct DTilemapProperty
{
    uint32_t nameOffset;   // offset into string pool
    uint32_t valueOffset;  // for String: string-pool offset; otherwise unused
    uint32_t type;         // DPropertyType
    union
    {
        int32_t intValue;
        float floatValue;
        uint32_t boolValue;
    };
};
#pragma pack(pop)

class Tilemap
{
public:
    static constexpr const char* kAssetTypeName = "Tilemap";

    static Tilemap* Load(const char* dtilemapPath);

    ~Tilemap();

    bool IsInfinite() const { return (m_MHeader.flags & 1u) != 0; }
    uint32_t MapWidth() const { return m_MHeader.mapWidth; }  // tiles; 0xFFFFFFFF if infinite
    uint32_t MapHeight() const { return m_MHeader.mapHeight; }
    uint16_t TileWidth() const { return m_MHeader.tileWidth; }
    uint16_t TileHeight() const { return m_MHeader.tileHeight; }
    uint16_t ChunkWidth() const { return m_MHeader.chunkWidth; }
    uint16_t ChunkHeight() const { return m_MHeader.chunkHeight; }
    uint32_t LayerCount() const { return m_MHeader.layerCount; }
    uint32_t BackgroundColor() const { return m_MHeader.backgroundColor; }

    const std::vector<TilesetRef>& Tilesets() const { return m_MTilesets; }

    // Resolve a global tile id to its tileset and local id. Returns nullptr if
    // gid is 0 or unmapped.
    const TilesetRef* ResolveTileset(uint32_t gid, uint32_t& outLocalId) const;

    // Same as ResolveTileset but also returns the tileset's index in
    // Tilesets() — saves the caller a linear search when iterating tiles.
    // Returns nullptr (and leaves outIndex untouched) for unmapped gids.
    const TilesetRef* ResolveTilesetWithIndex(uint32_t gid, uint32_t& outLocalId, size_t& outIndex) const;

    // Iterate the index for chunks intersecting the given chunk-coord rect on
    // the given layer. Output entries are guaranteed sorted by (cy, cx).
    void QueryVisibleChunks(int32_t layerIdx, int32_t chunkMinX, int32_t chunkMinY, int32_t chunkMaxX,
                            int32_t chunkMaxY, std::vector<ChunkIndexEntry>& out) const;

    TilemapStreamer* Streamer() const { return m_MStreamer; }

    // Object layers (loaded eagerly with the header).
    const std::vector<DObjectLayer>& ObjectLayers() const { return m_ObjectLayers; }
    const std::vector<DTilemapObject>& Objects() const { return m_MObjects; }

    // Tiled-pixel coordinate that should land on the owning GameObject. Set by
    // adding an object named "origin" (any layer, point or rect) in Tiled.
    // Returns false and leaves outX/outY untouched if no such object exists.
    // Resolved once at load (the render pass asks every frame).
    bool FindOrigin(float& outX, float& outY) const
    {
        if (!m_HasOrigin)
        {
            return false;
        }
        outX = m_OriginX;
        outY = m_OriginY;
        return true;
    }

    // Bounding box of authored chunks across all layers, in tile units.
    // For finite maps this is just (MapWidth, MapHeight). For infinite maps
    // it's derived from m_MIndex. Returns false if there are no chunks.
    // Resolved once at load (the render pass asks every frame).
    bool GetAuthoredBounds(int32_t& outMinTileX, int32_t& outMinTileY, int32_t& outWidthTiles,
                           int32_t& outHeightTiles) const
    {
        if (!m_HasBounds)
        {
            return false;
        }
        outMinTileX = m_BoundsMinX;
        outMinTileY = m_BoundsMinY;
        outWidthTiles = m_BoundsW;
        outHeightTiles = m_BoundsH;
        return true;
    }
    // Every object's properties, an object's being the propertyCount entries
    // from its propertyOffset. Names and string values are GetString offsets.
    const std::vector<DTilemapProperty>& Properties() const { return m_MProperties; }
    // Every polygon and polyline point as (x, y) pairs: an object's are the
    // pointCount pairs from pair pointOffset. Empty for maps baked before
    // the points were written.
    const std::vector<int32_t>& PolygonPoints() const { return m_PolygonPoints; }
    const std::string& StringPool() const { return m_StringPool; }

    // The properties of one object, or nullptr (and outCount 0) when it has
    // none or they are not in the file.
    const DTilemapProperty* ObjectProperties(const DTilemapObject& obj, uint32_t& outCount) const;

    // A string from the pool by its offset (empty if out of range).
    std::string GetString(uint32_t offset) const;

    // Internal — not intended for game code.
    const std::vector<ChunkIndexEntry>& Index() const { return m_MIndex; }
    const std::string& AbsolutePath() const { return m_AbsolutePath; }

private:
    Tilemap() = default;

    bool ComputeOrigin(float& outX, float& outY) const;
    bool ComputeAuthoredBounds(int32_t& outMinTileX, int32_t& outMinTileY, int32_t& outWidthTiles,
                               int32_t& outHeightTiles) const;

    bool m_HasOrigin = false;
    float m_OriginX = 0.0f, m_OriginY = 0.0f;
    bool m_HasBounds = false;
    int32_t m_BoundsMinX = 0, m_BoundsMinY = 0, m_BoundsW = 0, m_BoundsH = 0;

    DTilemapHeader m_MHeader{};
    std::vector<ChunkIndexEntry> m_MIndex;
    std::vector<TilesetRef> m_MTilesets;
    std::vector<DObjectLayer> m_ObjectLayers;
    std::vector<DTilemapObject> m_MObjects;
    std::vector<DTilemapProperty> m_MProperties;
    std::vector<int32_t> m_PolygonPoints;
    std::string m_StringPool;
    std::string m_AbsolutePath;
    TilemapStreamer* m_MStreamer = nullptr;
};

}  // namespace DekiTiledMap
