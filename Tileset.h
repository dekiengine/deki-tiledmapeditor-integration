#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "deki-2d/Sprite.h"
#include <deki/assets/AssetRef.h>

namespace DekiTiledMap
{

// On-disk header for .dtileset (100 bytes, little-endian, packed by hand).
// Version 1 files stop before margin, and have none of what follows it.
#pragma pack(push, 1)
struct DTilesetHeader
{
    char magic[4];       // 'D','T','S','1'
    uint32_t version;    // 1
    char atlasGuid[37];  // GUID of baked .dtex tileset image
    uint8_t pad0[3];
    uint16_t tileWidth;
    uint16_t tileHeight;
    uint16_t columns;
    uint16_t rows;
    uint32_t tileCount;
    uint32_t animTableOffset;
    uint32_t animCount;
    uint32_t collisionTableOffset;
    uint32_t collisionCount;
    uint32_t propertyTableOffset;
    uint32_t propertyCount;
    // Tiled "transparentcolor" chroma key. The high bit (0x80000000) turns it
    // on; the low 24 bits are RGB (R in bits 0-7, G in 8-15, B in 16-23, as in
    // ParseTiledColor's low 24 bits). 0 = no key. Older .dtileset files have
    // 0 here, which reads as off.
    uint32_t transparentColorFlag;
    // Tiled's margin (around the image) and spacing (between tiles), in pixels.
    uint16_t margin;
    uint16_t spacing;
    // Tiled's drawing offset for every tile of the set, in pixels (Y down).
    int16_t tileOffsetX;
    int16_t tileOffsetY;
    // TileObjectAlignment. A uint32 before render sizes were kept: its high
    // bytes were 0, which read as Tile Size and Stretch.
    uint8_t objectAlignment;
    uint8_t renderSize;  // Tiled's Tile Render Size: 0 Tile Size, 1 Map Grid Size
    uint8_t fillMode;    // with Map Grid Size: 0 Stretch, 1 Preserve Aspect Fit
    uint8_t pad1;
};
static_assert(sizeof(DTilesetHeader) == 100, "DTilesetHeader layout drift");
constexpr size_t kDTilesetHeaderV1Size = 88;

// Which point of a tile object's image sits on the object's position, from
// the tileset's Object Alignment. Unspecified means bottom-left on a square
// map and bottom-centre on an isometric one, as in Tiled.
enum class TileObjectAlignment : uint32_t
{
    Unspecified = 0,
    TopLeft,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};

// Per-tile animation: a tile's local id, and its frames with durations.
struct DTileAnimationFrame
{
    uint32_t localId;
    uint32_t durationMs;
};

struct DTileAnimation
{
    uint32_t localId;
    uint32_t frameOffset;  // file offset to DTileAnimationFrame[]
    uint32_t frameCount;
    uint32_t pad;
};

// Per-tile collision shape (single rect/ellipse/polygon).
enum class DTileCollisionShape : uint32_t
{
    Rect = 0,
    Ellipse = 1,
    Polygon = 2,
};

struct DTileCollision
{
    uint32_t localId;
    uint32_t shape;  // DTileCollisionShape
    int16_t x, y;
    uint16_t width, height;
    uint16_t pointCount;  // 0 for rect/ellipse
    uint16_t pad;
    uint32_t pointOffset;  // file offset to int16_t[2*pointCount] for polygons
};
#pragma pack(pop)

class Tileset
{
public:
    static constexpr const char* kAssetTypeName = "Tileset";

    /// Loads the header and tables. The atlas loads on first use, through
    /// AssetRef.
    static Tileset* Load(const char* dtilesetPath);

    ~Tileset();

    Deki2D::Sprite* Atlas() const;
    uint16_t TileWidth() const { return m_MHeader.tileWidth; }
    uint16_t TileHeight() const { return m_MHeader.tileHeight; }
    uint16_t Columns() const { return m_MHeader.columns; }
    uint16_t Rows() const { return m_MHeader.rows; }
    uint32_t TileCount() const { return m_MHeader.tileCount; }
    int TileOffsetX() const { return m_MHeader.tileOffsetX; }
    int TileOffsetY() const { return m_MHeader.tileOffsetY; }
    TileObjectAlignment ObjectAlignment() const { return static_cast<TileObjectAlignment>(m_MHeader.objectAlignment); }

    /// Tiled's Tile Render Size set to Map Grid Size: a tile layer draws this
    /// set's tiles at the map's tile size instead of their own.
    bool RendersAtGridSize() const { return m_MHeader.renderSize == 1; }
    /// With RendersAtGridSize: Fill Mode Preserve Aspect Fit, where a tile
    /// keeps its shape and is centred in the grid cell, instead of Stretch.
    bool PreservesAspect() const { return m_MHeader.fillMode == 1; }

    /// How big a tile layer draws this set's tiles on a map with tiles of
    /// mapTileWidth x mapTileHeight, in pixels, and where in the cell: its
    /// left and top, from the cell's left and bottom (the top is negative),
    /// before the set's Tile Offset. As Tiled's CellRenderer.
    void DrawnTileRect(int mapTileWidth, int mapTileHeight, float& outLeft, float& outTop, float& outWidth,
                       float& outHeight) const
    {
        const float tw = static_cast<float>(m_MHeader.tileWidth);
        const float th = static_cast<float>(m_MHeader.tileHeight);
        outLeft = 0.0f;
        outWidth = tw;
        outHeight = th;
        if (RendersAtGridSize() && tw > 0.0f && th > 0.0f)
        {
            outWidth = static_cast<float>(mapTileWidth);
            outHeight = static_cast<float>(mapTileHeight);
            if (PreservesAspect())
            {
                const float scale = std::min(outWidth / tw, outHeight / th);
                const float w = tw * scale;
                const float h = th * scale;
                outLeft = (outWidth - w) * 0.5f;
                // Centred in the cell, which stands on its bottom.
                outTop = -outHeight + (outHeight - h) * 0.5f;
                outWidth = w;
                outHeight = h;
                return;
            }
        }
        outTop = -outHeight;
    }

    /// Chroma key (Tiled "transparentcolor"), RGB only. The renderer skips
    /// matching pixels whatever the atlas's own alpha.
    bool HasTransparentColor() const { return (m_MHeader.transparentColorFlag & 0x80000000u) != 0; }
    uint8_t TransparentR() const { return m_MHeader.transparentColorFlag & 0xFFu; }
    uint8_t TransparentG() const { return (m_MHeader.transparentColorFlag >> 8) & 0xFFu; }
    uint8_t TransparentB() const { return (m_MHeader.transparentColorFlag >> 16) & 0xFFu; }

    /// The rect in the atlas of a tile, by local id.
    void GetTileRect(uint32_t localId, int& x, int& y, int& w, int& h) const;

    const DTileAnimation* GetAnimation(uint32_t localId) const;
    bool HasAnimations() const { return !m_MAnims.empty(); }
    const DTileCollision* GetCollision(uint32_t localId) const;
    const DTileAnimationFrame* GetAnimationFrames(const DTileAnimation& a) const;

private:
    Tileset() = default;

    DTilesetHeader m_MHeader{};
    mutable Deki::AssetRef<Deki2D::Sprite> m_MAtlas;
    std::vector<DTileAnimation> m_MAnims;
    std::vector<DTileAnimationFrame> m_AnimFrames;
    std::vector<DTileCollision> m_MCollisions;
};

/// Registers the Tileset asset loader. Safe to call more than once; see
/// DekiTilemapInitSystem.
void RegisterTilesetLoader();

}  // namespace DekiTiledMap
