#pragma once

#include <cmath>
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
    uint32_t version;   // 1; 2 adds DTilemapHeaderV2 after it; 3 stores object positions as floats
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
    uint32_t flags;            // bit0 = infinite, then the kTilemap* flags below
    // Where the per-object pools are; valid when flags has kTilemapHasPools.
    uint32_t pointPoolOffset;      // int32_t[2 * pointPoolCount], (x, y) pairs
    uint32_t pointPoolCount;       // points
    uint32_t propertyTableOffset;  // DTilemapProperty[propertyCount]
    uint32_t propertyCount;
    uint32_t stringPoolOffset;  // runs to the end of the file
};
static_assert(sizeof(DTilemapHeader) == 80, "DTilemapHeader layout drift");

constexpr uint32_t kTilemapHasPools = 1u << 1;
constexpr uint32_t kTilemapIsometric = 1u << 2;
// Tiled's staggered and hexagonal maps (version 3): every other row, or with
// kTilemapStaggerX every other column, is shifted by half a tile.
constexpr uint32_t kTilemapStaggered = 1u << 3;
constexpr uint32_t kTilemapHexagonal = 1u << 4;
constexpr uint32_t kTilemapStaggerX = 1u << 5;     // columns are shifted, not rows
constexpr uint32_t kTilemapStaggerEven = 1u << 6;  // the even ones, not the odd ones

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
    float x, y;  // pixels, with Tiled's fractions (whole pixels before version 3)
    float width, height;
    float rotation;        // degrees, Tiled convention (converted to radians at spawn time)
    uint32_t pointOffset;  // first point in PolygonPoints(), in points (polygons, polylines)
    uint32_t pointCount;
    uint32_t propertyOffset;  // first entry in Properties()
    uint32_t propertyCount;
    char name[32];
    char type[32];  // Tiled's object class
};

// Version 2 puts this right after the header.
struct DTilemapHeaderV2
{
    uint32_t layerTableOffset;  // DLayerLook[layerTableCount]
    uint32_t layerTableCount;
    float parallaxOriginX;  // map pixels, Tiled's Parallax Origin
    float parallaxOriginY;
};
static_assert(sizeof(DTilemapHeaderV2) == 16, "DTilemapHeaderV2 layout drift");

// Version 3 adds this after DTilemapHeaderV2.
struct DTilemapHeaderV3
{
    uint32_t hexSideLength;  // a hexagonal map's Hex Side Length, in pixels
    uint32_t reserved[3];
};
static_assert(sizeof(DTilemapHeaderV3) == 16, "DTilemapHeaderV3 layout drift");

enum class DLayerKind : uint8_t
{
    Tile = 0,
    Object = 1,
    Image = 2,
};
constexpr uint8_t kLayerRepeatX = 1u << 0;  // image layers
constexpr uint8_t kLayerRepeatY = 1u << 1;
constexpr uint8_t kLayerHidden = 1u << 2;

// One layer of the map as Tiled draws it. The layer table lists every layer
// bottom first; groups are flattened, their settings folded into their layers.
struct DLayerLook
{
    uint8_t kind;    // DLayerKind
    uint8_t flags;   // kLayerRepeatX, kLayerRepeatY, kLayerHidden
    uint16_t index;  // which tile layer, object layer or image layer
    float opacity;   // 0 to 1
    uint32_t tint;   // RGBA8, R in the low byte; 0xFFFFFFFF leaves colours as they are
    float offsetX;   // map pixels, along the screen
    float offsetY;
    float parallaxX;  // 1 moves with the map, 0 stays with the camera
    float parallaxY;
    char imageGuid[37];  // an image layer's image
    uint8_t blendMode;   // DBlendMode
    uint8_t pad[2];
};
static_assert(sizeof(DLayerLook) == 68, "DLayerLook layout drift");

// Tiled's layer blend modes (a layer's Mode), in Tiled's order.
enum class DBlendMode : uint8_t
{
    Normal = 0,
    Add,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
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
    bool IsIsometric() const { return (m_MHeader.flags & kTilemapIsometric) != 0; }
    /// A staggered or a hexagonal map: rows (or columns) shifted by half a tile.
    bool IsStaggered() const { return (m_MHeader.flags & (kTilemapStaggered | kTilemapHexagonal)) != 0; }
    bool IsHexagonal() const { return (m_MHeader.flags & kTilemapHexagonal) != 0; }

    /// How Tiled lays out a staggered or hexagonal map's tiles, in pixels
    /// (its HexagonalRenderer::RenderParams). A staggered map is a hexagonal
    /// one whose sides have no length.
    struct StaggerGrid
    {
        bool staggerX = false;     // columns are shifted, not rows
        bool staggerEven = false;  // the even ones, not the odd ones
        int32_t tileWidth = 0;     // the map's tile size, made even
        int32_t tileHeight = 0;
        int32_t sideLengthX = 0;
        int32_t sideLengthY = 0;
        int32_t sideOffsetX = 0;
        int32_t sideOffsetY = 0;
        int32_t columnWidth = 0;
        int32_t rowHeight = 0;

        /// Whether a row (or with staggerX a column) is the shifted kind.
        bool Shifted(int32_t index) const { return ((index & 1) != 0) != staggerEven; }

        /// The top-left of a tile's box, in map pixels.
        void TileCorner(int32_t column, int32_t row, int32_t& outX, int32_t& outY) const
        {
            if (staggerX)
            {
                outX = column * columnWidth;
                outY = row * (tileHeight + sideLengthY) + (Shifted(column) ? rowHeight : 0);
                return;
            }
            outX = column * (tileWidth + sideLengthX) + (Shifted(row) ? columnWidth : 0);
            outY = row * rowHeight;
        }

        /// The tile whose shape holds a point in map pixels, as Tiled finds it.
        void TileAt(float x, float y, bool hexagonal, int32_t& outColumn, int32_t& outRow) const;
    };
    /// A staggered or hexagonal map's grid; all zero on other maps.
    const StaggerGrid& Stagger() const { return m_Stagger; }
    uint32_t MapWidth() const { return m_MHeader.mapWidth; }  // tiles; 0xFFFFFFFF if infinite
    uint32_t MapHeight() const { return m_MHeader.mapHeight; }
    uint16_t TileWidth() const { return m_MHeader.tileWidth; }
    uint16_t TileHeight() const { return m_MHeader.tileHeight; }
    uint16_t ChunkWidth() const { return m_MHeader.chunkWidth; }
    uint16_t ChunkHeight() const { return m_MHeader.chunkHeight; }
    uint32_t LayerCount() const { return m_MHeader.layerCount; }
    uint32_t BackgroundColor() const { return m_MHeader.backgroundColor; }

    const std::vector<TilesetRef>& Tilesets() const { return m_MTilesets; }

    /// One bit per tile layer hidden in Tiled. Hidden layers are not drawn, and
    /// collision can still read them.
    uint32_t HiddenLayerMask() const { return m_HiddenLayerMask; }

    /// The tileset and local id of a global tile id, or nullptr if gid is 0 or
    /// in no tileset.
    const TilesetRef* ResolveTileset(uint32_t gid, uint32_t& outLocalId) const;

    /// Like ResolveTileset, and also gives the tileset's index in Tilesets(),
    /// which saves a search per tile. Returns nullptr, leaving outIndex as it
    /// was, for a gid in no tileset.
    const TilesetRef* ResolveTilesetWithIndex(uint32_t gid, uint32_t& outLocalId, size_t& outIndex) const;

    /// The index entries of the chunks on a layer that intersect a rect in
    /// chunk coordinates, sorted by (cy, cx).
    void QueryVisibleChunks(int32_t layerIdx, int32_t chunkMinX, int32_t chunkMinY, int32_t chunkMaxX,
                            int32_t chunkMaxY, std::vector<ChunkIndexEntry>& out) const;

    TilemapStreamer* Streamer() const { return m_MStreamer; }

    /// Object layers, loaded with the header.
    const std::vector<DObjectLayer>& ObjectLayers() const { return m_ObjectLayers; }
    const std::vector<DTilemapObject>& Objects() const { return m_MObjects; }

    /// Which object layer an object of Objects() is on.
    uint32_t ObjectLayerOf(const DTilemapObject& obj) const;

    /// Every layer in Tiled's drawing order, bottom first, with its opacity,
    /// tint, offset and parallax. Empty for a map baked before these were kept.
    const std::vector<DLayerLook>& Layers() const { return m_Layers; }

    /// How a tile, object or image layer is drawn, or nullptr when the map
    /// does not say (it is then drawn plain).
    const DLayerLook* FindLayerLook(DLayerKind kind, uint32_t index) const;

    /// Tiled's Parallax Origin, in map pixels. A layer with parallax moves by
    /// (1 - factor) times the view's centre plus this, as in Tiled.
    float ParallaxOriginX() const { return m_ParallaxOriginX; }
    float ParallaxOriginY() const { return m_ParallaxOriginY; }

    /// The point, in Tiled pixels, that sits on the owning object. Set by
    /// adding an object named "origin" (any layer, point or rect) in Tiled.
    /// Returns false, leaving outX/outY as they were, when there is none.
    /// Found once at load, since the render pass asks every frame.
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

    /// Where a point of the tile grid is, in map pixels (Y down). A whole
    /// column and row give a tile's top-left corner, or on an isometric map
    /// the top point of its diamond, or on a staggered or hexagonal map the
    /// top-left of its box (shifted rows included; a fraction moves on by
    /// that much of a step).
    void TileToPixels(float column, float row, float& outX, float& outY) const
    {
        const float tw = static_cast<float>(m_MHeader.tileWidth);
        const float th = static_cast<float>(m_MHeader.tileHeight);
        if (IsStaggered())
        {
            const float wholeColumn = std::floor(column);
            const float wholeRow = std::floor(row);
            int32_t x = 0, y = 0;
            m_Stagger.TileCorner(static_cast<int32_t>(wholeColumn), static_cast<int32_t>(wholeRow), x, y);
            const StaggerGrid& g = m_Stagger;
            const float stepX = static_cast<float>(g.staggerX ? g.columnWidth : g.tileWidth + g.sideLengthX);
            const float stepY = static_cast<float>(g.staggerX ? g.tileHeight + g.sideLengthY : g.rowHeight);
            outX = static_cast<float>(x) + (column - wholeColumn) * stepX;
            outY = static_cast<float>(y) + (row - wholeRow) * stepY;
            return;
        }
        if (IsIsometric())
        {
            outX = (column - row) * tw * 0.5f;
            outY = (column + row) * th * 0.5f;
            return;
        }
        outX = column * tw;
        outY = row * th;
    }

    /// The point of the tile grid at some map pixels: TileToPixels undone. On
    /// a staggered or hexagonal map, whole numbers: the tile whose shape holds
    /// the point, as Tiled picks it.
    void PixelsToTile(float x, float y, float& outColumn, float& outRow) const
    {
        const float tw = static_cast<float>(m_MHeader.tileWidth);
        const float th = static_cast<float>(m_MHeader.tileHeight);
        if (IsStaggered())
        {
            int32_t column = 0, row = 0;
            m_Stagger.TileAt(x, y, IsHexagonal(), column, row);
            outColumn = static_cast<float>(column);
            outRow = static_cast<float>(row);
            return;
        }
        if (IsIsometric())
        {
            const float across = x / (tw * 0.5f);
            const float down = y / (th * 0.5f);
            outColumn = (down + across) * 0.5f;
            outRow = (down - across) * 0.5f;
            return;
        }
        outColumn = x / tw;
        outRow = y / th;
    }

    /// Where a map object is, in map pixels. Tiled stores an isometric map's
    /// objects along the grid's axes, at a tile's height per tile on both.
    void ObjectToPixels(float objectX, float objectY, float& outX, float& outY) const
    {
        if (IsIsometric())
        {
            const float th = static_cast<float>(m_MHeader.tileHeight);
            TileToPixels(objectX / th, objectY / th, outX, outY);
            return;
        }
        outX = objectX;
        outY = objectY;
    }

    /// A finite map's size in pixels, as Tiled works it out for the image it
    /// renders. False for an infinite map.
    bool MapPixelSize(int32_t& outWidth, int32_t& outHeight) const;

    /// How far left of the first tile's corner Tiled's 0, 0 is, in map pixels:
    /// on a finite isometric map the top-left of its bounds is half its height
    /// in tiles across to the left. 0 on other maps.
    float BoundsLeft() const
    {
        return IsIsometric() && !IsInfinite()
                   ? 0.5f * static_cast<float>(m_MHeader.mapHeight) * static_cast<float>(m_MHeader.tileWidth)
                   : 0.0f;
    }

    /// The map point, in map pixels, that sits on the owning object: a finite
    /// map's centre, or an infinite map's origin object, or (0, 0).
    void GetAnchor(float& outX, float& outY) const
    {
        outX = 0.0f;
        outY = 0.0f;
        if (!IsInfinite() && IsStaggered())
        {
            int32_t w = 0, h = 0;
            MapPixelSize(w, h);
            outX = 0.5f * static_cast<float>(w);
            outY = 0.5f * static_cast<float>(h);
            return;
        }
        if (!IsInfinite())
        {
            TileToPixels(0.5f * static_cast<float>(m_MHeader.mapWidth), 0.5f * static_cast<float>(m_MHeader.mapHeight),
                         outX, outY);
            return;
        }
        FindOrigin(outX, outY);
    }

    /// Where the middle of a tile object's image is, in map pixels. Tiled
    /// stands the image on the object's point: by its tileset's alignment, or
    /// else bottom-centre on an isometric map and bottom-left on a square one,
    /// turning with the object around that point.
    void TileObjectCentre(const DTilemapObject& obj, float width, float height, TileObjectAlignment alignment,
                          float& outX, float& outY) const
    {
        float px = 0.0f, py = 0.0f;
        ObjectToPixels(static_cast<float>(obj.x), static_cast<float>(obj.y), px, py);
        // Tiled draws an object on a half pixel at the pixel above it.
        px = std::floor(px);
        py = std::floor(py);

        // How far across and down the image its point is, from 0 to 1.
        float across = IsIsometric() ? 0.5f : 0.0f;
        float down = 1.0f;
        switch (alignment)
        {
            case TileObjectAlignment::TopLeft: across = 0.0f; down = 0.0f; break;
            case TileObjectAlignment::Top: across = 0.5f; down = 0.0f; break;
            case TileObjectAlignment::TopRight: across = 1.0f; down = 0.0f; break;
            case TileObjectAlignment::Left: across = 0.0f; down = 0.5f; break;
            case TileObjectAlignment::Center: across = 0.5f; down = 0.5f; break;
            case TileObjectAlignment::Right: across = 1.0f; down = 0.5f; break;
            case TileObjectAlignment::BottomLeft: across = 0.0f; down = 1.0f; break;
            case TileObjectAlignment::Bottom: across = 0.5f; down = 1.0f; break;
            case TileObjectAlignment::BottomRight: across = 1.0f; down = 1.0f; break;
            case TileObjectAlignment::Unspecified: break;
        }
        const float dx = (0.5f - across) * width;
        const float dy = (0.5f - down) * height;
        const float radians = obj.rotation * 3.14159265f / 180.0f;
        outX = px + dx * std::cos(radians) - dy * std::sin(radians);
        outY = py + dx * std::sin(radians) + dy * std::cos(radians);
    }

    /// The bounding box of all layers' chunks, in tiles: (MapWidth,
    /// MapHeight) for a finite map, from the chunk index for an infinite one.
    /// Returns false if there are no chunks. Found once at load, since the
    /// render pass asks every frame.
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
    /// Every object's properties; an object's are the propertyCount entries
    /// from its propertyOffset. Names and string values are GetString offsets.
    const std::vector<DTilemapProperty>& Properties() const { return m_MProperties; }
    /// Every polygon and polyline point as (x, y) pairs; an object's are the
    /// pointCount pairs from pair pointOffset. Empty for older maps that do
    /// not store points.
    const std::vector<int32_t>& PolygonPoints() const { return m_PolygonPoints; }
    const std::string& StringPool() const { return m_StringPool; }

    /// The properties of one object, or nullptr (and outCount 0) when it has
    /// none or they are not in the file.
    const DTilemapProperty* ObjectProperties(const DTilemapObject& obj, uint32_t& outCount) const;

    /// One of an object's custom properties by its name in Tiled, or nullptr
    /// when it has none by that name.
    const DTilemapProperty* FindProperty(const DTilemapObject& obj, const char* name) const;

    /// A string from the pool by its offset (empty if out of range).
    std::string GetString(uint32_t offset) const;

    // Internal, not for game code.
    const std::vector<ChunkIndexEntry>& Index() const { return m_MIndex; }
    const std::string& AbsolutePath() const { return m_AbsolutePath; }

private:
    Tilemap() = default;

    bool ComputeOrigin(float& outX, float& outY) const;
    bool ComputeAuthoredBounds(int32_t& outMinTileX, int32_t& outMinTileY, int32_t& outWidthTiles,
                               int32_t& outHeightTiles) const;

    uint32_t m_HiddenLayerMask = 0;
    StaggerGrid m_Stagger;
    bool m_HasOrigin = false;
    float m_OriginX = 0.0f, m_OriginY = 0.0f;
    bool m_HasBounds = false;
    int32_t m_BoundsMinX = 0, m_BoundsMinY = 0, m_BoundsW = 0, m_BoundsH = 0;

    DTilemapHeader m_MHeader{};
    std::vector<ChunkIndexEntry> m_MIndex;
    std::vector<TilesetRef> m_MTilesets;
    std::vector<DObjectLayer> m_ObjectLayers;
    std::vector<DTilemapObject> m_MObjects;
    std::vector<DLayerLook> m_Layers;
    float m_ParallaxOriginX = 0.0f, m_ParallaxOriginY = 0.0f;
    std::vector<DTilemapProperty> m_MProperties;
    std::vector<int32_t> m_PolygonPoints;
    std::string m_StringPool;
    std::string m_AbsolutePath;
    TilemapStreamer* m_MStreamer = nullptr;
};

/// Registers the Tilemap asset loader. Safe to call more than once; see
/// DekiTilemapInitSystem.
void RegisterTilemapLoader();

}  // namespace DekiTiledMap
