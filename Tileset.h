#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "deki-2d/Sprite.h"
#include "assets/AssetRef.h"

namespace DekiTilemap
{

// On-disk header for .dtileset (88 bytes, little-endian, packed by hand).
#pragma pack(push, 1)
struct DTilesetHeader
{
    char     magic[4];          // 'D','T','S','1'
    uint32_t version;           // 1
    char     atlasGuid[37];     // GUID of baked .dtex tileset image
    uint8_t  pad0[3];
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
    // Tiled "transparentcolor" chroma key. High bit (0x80000000) is the active
    // flag, low 24 bits hold packed RGB (R in bits 0-7, G in 8-15, B in 16-23
    // — matches ParseTiledColor's low 24 bits). 0 = no key. Older .dtileset
    // files written before this field have pad=0 here, which decodes as "off".
    uint32_t transparentColorFlag;
};
static_assert(sizeof(DTilesetHeader) == 88, "DTilesetHeader layout drift");

// Per-tile animation: tileGids[localId] -> {duration_ms, frames[]}
struct DTileAnimationFrame
{
    uint32_t localId;
    uint32_t durationMs;
};

struct DTileAnimation
{
    uint32_t localId;
    uint32_t frameOffset;   // file offset to DTileAnimationFrame[]
    uint32_t frameCount;
    uint32_t pad;
};

// Per-tile collision shape (single rect/ellipse/polygon).
enum class DTileCollisionShape : uint32_t
{
    Rect    = 0,
    Ellipse = 1,
    Polygon = 2,
};

struct DTileCollision
{
    uint32_t localId;
    uint32_t shape;        // DTileCollisionShape
    int16_t  x, y;
    uint16_t width, height;
    uint16_t pointCount;   // 0 for rect/ellipse
    uint16_t pad;
    uint32_t pointOffset;  // file offset to int16_t[2*pointCount] for polygons
};
#pragma pack(pop)

class Tileset
{
public:
    static constexpr const char* AssetTypeName = "Tileset";

    // Loads header + index tables. Atlas is loaded lazily through AssetRef.
    static Tileset* Load(const char* dtilesetPath);

    ~Tileset();

    Sprite*  Atlas() const;
    uint16_t TileWidth()  const { return m_MHeader.tileWidth;  }
    uint16_t TileHeight() const { return m_MHeader.tileHeight; }
    uint16_t Columns()    const { return m_MHeader.columns;    }
    uint16_t Rows()       const { return m_MHeader.rows;       }
    uint32_t TileCount()  const { return m_MHeader.tileCount;  }

    // Chroma-key (Tiled "transparentcolor"). RGB-only; renderer skips matching
    // pixels regardless of the atlas's own alpha channel (or lack thereof).
    bool    HasTransparentColor() const { return (m_MHeader.transparentColorFlag & 0x80000000u) != 0; }
    uint8_t TransparentR()        const { return  m_MHeader.transparentColorFlag        & 0xFFu; }
    uint8_t TransparentG()        const { return (m_MHeader.transparentColorFlag >>  8) & 0xFFu; }
    uint8_t TransparentB()        const { return (m_MHeader.transparentColorFlag >> 16) & 0xFFu; }

    // Compute the source rect inside the atlas for a tile local id.
    void GetTileRect(uint32_t localId, int& x, int& y, int& w, int& h) const;

    const DTileAnimation* GetAnimation(uint32_t localId) const;
    const DTileCollision* GetCollision(uint32_t localId) const;
    const DTileAnimationFrame* GetAnimationFrames(const DTileAnimation& a) const;

private:
    Tileset() = default;

    DTilesetHeader m_MHeader{};
    mutable Deki::AssetRef<Sprite> m_MAtlas;
    std::vector<DTileAnimation>      m_MAnims;
    std::vector<DTileAnimationFrame> m_animFrames;
    std::vector<DTileCollision>      m_MCollisions;
};

} // namespace DekiTilemap
