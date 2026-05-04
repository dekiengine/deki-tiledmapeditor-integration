#ifdef DEKI_EDITOR

#include "TilemapBaker.h"
#include "Tilemap.h"
#include "Tileset.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "DekiLogSystem.h"

namespace DekiTilemap
{

namespace
{

// Append helper that records the file offset of where data was written.
template <typename T>
uint32_t AppendBlob(FILE* f, const T* src, size_t count)
{
    uint32_t off = static_cast<uint32_t>(std::ftell(f));
    if (count > 0)
        std::fwrite(src, sizeof(T), count, f);
    return off;
}

uint32_t AppendString(FILE* f, const std::string& s)
{
    uint32_t off = static_cast<uint32_t>(std::ftell(f));
    std::fwrite(s.data(), 1, s.size(), f);
    char zero = 0;
    std::fwrite(&zero, 1, 1, f);
    return off;
}

void CopyName32(char dst[32], const std::string& src)
{
    std::memset(dst, 0, 32);
    std::memcpy(dst, src.data(), std::min<size_t>(31, src.size()));
}

} // namespace

bool WriteDtileset(const TmjTileset& ts,
                   const std::string& atlasGuid,
                   const std::string& outAbsPath)
{
    FILE* f = std::fopen(outAbsPath.c_str(), "wb");
    if (!f)
    {
        DEKI_LOG_ERROR("TilemapBaker: cannot open '%s' for writing", outAbsPath.c_str());
        return false;
    }

    DTilesetHeader hdr{};
    std::memcpy(hdr.magic, "DTS1", 4);
    hdr.version    = 1;
    std::memset(hdr.atlasGuid, 0, sizeof(hdr.atlasGuid));
    std::memcpy(hdr.atlasGuid, atlasGuid.data(),
                std::min<size_t>(36, atlasGuid.size()));
    hdr.tileWidth  = static_cast<uint16_t>(ts.tileWidth);
    hdr.tileHeight = static_cast<uint16_t>(ts.tileHeight);
    hdr.columns    = static_cast<uint16_t>(ts.columns);
    hdr.rows       = static_cast<uint16_t>(ts.rows);
    hdr.tileCount  = static_cast<uint32_t>(ts.tileCount);

    // Encode chroma key: high bit = active, low 24 bits = packed RGB. The
    // alpha byte from ParseTiledColor (high byte of the RGBA packing) is
    // dropped — we only ever match on RGB.
    hdr.transparentColorFlag = ts.hasTransparentColor
        ? (0x80000000u | (ts.transparentColor & 0x00FFFFFFu))
        : 0u;

    // Reserve header space, fill offsets later.
    std::fwrite(&hdr, sizeof(hdr), 1, f);

    // Build animation table + frames.
    std::vector<DTileAnimation>      anims;
    std::vector<DTileAnimationFrame> frames;
    for (const auto& tile : ts.tiles)
    {
        if (tile.animation.empty()) continue;
        DTileAnimation a{};
        a.localId     = tile.id;
        a.frameOffset = 0;   // patched after we know the frames base
        a.frameCount  = static_cast<uint32_t>(tile.animation.size());
        a.pad         = 0;
        anims.push_back(a);
        for (const auto& fr : tile.animation)
            frames.push_back({fr.tileId, fr.durationMs});
    }

    // Build collision table.
    std::vector<DTileCollision> collisions;
    std::vector<int32_t>        colPolyPoints;
    for (const auto& tile : ts.tiles)
    {
        if (!tile.hasCollision) continue;
        DTileCollision c{};
        c.localId = tile.id;
        c.shape   = tile.collisionShape;
        c.x       = static_cast<int16_t>(tile.cx);
        c.y       = static_cast<int16_t>(tile.cy);
        c.width   = static_cast<uint16_t>(tile.cw);
        c.height  = static_cast<uint16_t>(tile.ch);
        c.pointCount  = static_cast<uint16_t>(tile.collisionPolygon.size() / 2);
        c.pointOffset = 0;   // patched once we know the polygon blob offset
        collisions.push_back(c);
    }

    hdr.animTableOffset      = AppendBlob(f, anims.data(), anims.size());
    hdr.animCount            = static_cast<uint32_t>(anims.size());
    uint32_t framesBase      = AppendBlob(f, frames.data(), frames.size());

    // Patch animation frame offsets.
    if (!anims.empty())
    {
        std::fseek(f, static_cast<long>(hdr.animTableOffset), SEEK_SET);
        uint32_t cursor = framesBase;
        for (auto& a : anims)
        {
            a.frameOffset = cursor;
            cursor += a.frameCount * sizeof(DTileAnimationFrame);
        }
        std::fwrite(anims.data(), sizeof(DTileAnimation), anims.size(), f);
        std::fseek(f, 0, SEEK_END);
    }

    hdr.collisionTableOffset = AppendBlob(f, collisions.data(), collisions.size());
    hdr.collisionCount       = static_cast<uint32_t>(collisions.size());

    // Polygon points (currently unreferenced from collision rows in v1 — collision
    // shapes are bounding-rect tested at runtime; storing the points keeps the
    // file format forward-compatible).
    hdr.propertyTableOffset = AppendBlob(f, colPolyPoints.data(), colPolyPoints.size());
    hdr.propertyCount       = 0;

    // Rewrite header with patched offsets.
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&hdr, sizeof(hdr), 1, f);

    std::fclose(f);
    return true;
}

bool WriteDtilemap(const TmjMap& map,
                   const std::vector<BakedTilesetRef>& tilesets,
                   const std::string& outAbsPath)
{
    FILE* f = std::fopen(outAbsPath.c_str(), "wb");
    if (!f)
    {
        DEKI_LOG_ERROR("TilemapBaker: cannot open '%s' for writing", outAbsPath.c_str());
        return false;
    }

    DTilemapHeader hdr{};
    std::memcpy(hdr.magic, "DTM1", 4);
    hdr.version          = 1;
    hdr.mapWidth         = map.infinite ? 0xFFFFFFFFu : static_cast<uint32_t>(map.width);
    hdr.mapHeight        = map.infinite ? 0xFFFFFFFFu : static_cast<uint32_t>(map.height);
    hdr.tileWidth        = static_cast<uint16_t>(map.tileWidth);
    hdr.tileHeight       = static_cast<uint16_t>(map.tileHeight);
    hdr.chunkWidth       = static_cast<uint16_t>(map.chunkWidth);
    hdr.chunkHeight      = static_cast<uint16_t>(map.chunkHeight);
    hdr.layerCount       = static_cast<uint32_t>(map.tileLayers.size());
    hdr.backgroundColor  = map.backgroundColor;
    hdr.flags            = map.infinite ? 1u : 0u;

    std::fwrite(&hdr, sizeof(hdr), 1, f);

    // Tileset table.
    std::vector<TilesetRef> tsRefs;
    tsRefs.reserve(tilesets.size());
    for (const auto& t : tilesets)
    {
        TilesetRef r{};
        std::memset(r.guid, 0, sizeof(r.guid));
        std::memcpy(r.guid, t.guid.data(), std::min<size_t>(36, t.guid.size()));
        r.firstGid = t.firstGid;
        tsRefs.push_back(r);
    }
    hdr.tilesetTableOffset = AppendBlob(f, tsRefs.data(), tsRefs.size());
    hdr.tilesetCount       = static_cast<uint32_t>(tsRefs.size());

    // Build chunk index + payloads. Single pass: write chunk payloads inline,
    // remember each (offset, ChunkIndexEntry), then write index after.
    struct PendingChunk
    {
        ChunkIndexEntry      entry;
        std::vector<uint32_t> payload;
    };
    std::vector<PendingChunk> pending;

    auto pushChunkData = [&](uint32_t layerIndex, int32_t cx, int32_t cy,
                             const std::vector<uint32_t>& tiles)
    {
        if (tiles.empty()) return;

        // Pad/truncate to chunkW * chunkH for the on-disk payload.
        const size_t want = static_cast<size_t>(map.chunkWidth) * map.chunkHeight;
        std::vector<uint32_t> payload(want, 0);
        const size_t copy = std::min(want, tiles.size());
        std::memcpy(payload.data(), tiles.data(), copy * 4);

        // Empty / uniform-fill compression heuristics.
        bool empty = true, uniform = true;
        uint32_t first = payload[0];
        for (uint32_t v : payload) { if (v != 0) { empty = false; } if (v != first) { uniform = false; } }

        ChunkIndexEntry e{};
        e.chunkX     = cx;
        e.chunkY     = cy;
        e.layerIndex = static_cast<uint16_t>(layerIndex);
        e.flags      = 0;
        if (empty)        e.flags |= CHUNK_FLAG_EMPTY;
        else if (uniform) e.flags |= CHUNK_FLAG_UNIFORM_FILL;

        PendingChunk pc;
        pc.entry = e;
        if (e.flags & CHUNK_FLAG_EMPTY) {
            // no payload bytes
        } else if (e.flags & CHUNK_FLAG_UNIFORM_FILL) {
            pc.payload = { first };
        } else {
            pc.payload = std::move(payload);
        }
        pending.push_back(std::move(pc));
    };

    for (uint32_t layer = 0; layer < map.tileLayers.size(); ++layer)
    {
        const auto& L = map.tileLayers[layer];
        if (map.infinite)
        {
            for (const auto& chunk : L.chunks)
            {
                // Tiled chunk coords are in *tiles*; convert to chunk-grid coords.
                if (map.chunkWidth <= 0 || map.chunkHeight <= 0) continue;
                const int32_t cx = chunk.x / map.chunkWidth;
                const int32_t cy = chunk.y / map.chunkHeight;
                pushChunkData(layer, cx, cy, chunk.data);
            }
        }
        else
        {
            // Slice the finite layer into chunks.
            const int W = L.width, H = L.height;
            const int CW = map.chunkWidth, CH = map.chunkHeight;
            const int cxN = (W + CW - 1) / CW;
            const int cyN = (H + CH - 1) / CH;
            for (int cy = 0; cy < cyN; ++cy)
            for (int cx = 0; cx < cxN; ++cx)
            {
                std::vector<uint32_t> tiles(static_cast<size_t>(CW) * CH, 0);
                for (int ty = 0; ty < CH; ++ty)
                for (int tx = 0; tx < CW; ++tx)
                {
                    int gx = cx * CW + tx;
                    int gy = cy * CH + ty;
                    if (gx >= W || gy >= H) continue;
                    tiles[ty * CW + tx] = L.data[static_cast<size_t>(gy) * W + gx];
                }
                pushChunkData(layer, cx, cy, tiles);
            }
        }
    }

    // Write payloads, fix entry.payloadOffset.
    for (auto& pc : pending)
    {
        if (pc.payload.empty())
            pc.entry.payloadOffset = 0;
        else
            pc.entry.payloadOffset = AppendBlob(f, pc.payload.data(), pc.payload.size());
    }

    // Write chunk index.
    std::vector<ChunkIndexEntry> indexRows;
    indexRows.reserve(pending.size());
    for (auto& pc : pending) indexRows.push_back(pc.entry);
    hdr.chunkIndexOffset = AppendBlob(f, indexRows.data(), indexRows.size());
    hdr.chunkIndexCount  = static_cast<uint32_t>(indexRows.size());

    // Write object layer table + flat object list.
    std::vector<DObjectLayer>    olRows;
    std::vector<DTilemapObject>  objRows;
    std::vector<int32_t>         polyPoints;
    std::vector<DTilemapProperty> propRows;
    std::string                   stringPool;

    auto pushString = [&](const std::string& s) -> uint32_t {
        uint32_t off = static_cast<uint32_t>(stringPool.size());
        stringPool.append(s);
        stringPool.push_back('\0');
        return off;
    };

    for (const auto& OL : map.objectLayers)
    {
        DObjectLayer L{};
        CopyName32(L.name, OL.name);
        L.objectOffset = 0;          // patched after we know objects file offset
        L.objectCount  = static_cast<uint32_t>(OL.objects.size());
        olRows.push_back(L);

        const uint32_t firstObjIdx = static_cast<uint32_t>(objRows.size());
        for (const auto& o : OL.objects)
        {
            DTilemapObject row{};
            row.id    = o.id;
            row.gid   = o.gid;
            row.x     = o.x;
            row.y     = o.y;
            row.width = o.width;
            row.height= o.height;
            row.rotation = o.rotation;
            CopyName32(row.name, o.name);
            CopyName32(row.type, o.type);

            uint32_t shape = 0;   // Rect
            if (o.gid != 0)               shape = 4;   // Tile
            else if (o.ellipse)           shape = 1;   // Ellipse
            else if (!o.polygonPoints.empty())  shape = 2;   // Polygon
            else if (!o.polylinePoints.empty()) shape = 3;   // Polyline
            row.shape = shape;

            row.pointCount  = static_cast<uint32_t>(
                (shape == 2 ? o.polygonPoints.size() : o.polylinePoints.size()) / 2);
            row.pointOffset = 0;        // patched after the polygon blob is written

            row.propertyOffset = static_cast<uint32_t>(propRows.size());
            row.propertyCount  = static_cast<uint32_t>(o.properties.size());

            for (const auto& p : o.properties)
            {
                DTilemapProperty prop{};
                prop.nameOffset = pushString(p.name);
                if (p.type == "int")        { prop.type = 1; prop.intValue   = p.ivalue; }
                else if (p.type == "float") { prop.type = 2; prop.floatValue = p.fvalue; }
                else if (p.type == "bool")  { prop.type = 3; prop.boolValue  = p.bvalue ? 1u : 0u; }
                else                        { prop.type = 0; prop.valueOffset = pushString(p.svalue); }
                propRows.push_back(prop);
            }

            objRows.push_back(row);
        }
        // Patch this layer's objectOffset later (we don't know it yet).
        olRows.back().objectOffset = firstObjIdx;   // temporarily store the index
    }

    hdr.objectLayerOffset = AppendBlob(f, olRows.data(), olRows.size());
    hdr.objectLayerCount  = static_cast<uint32_t>(olRows.size());

    // Now write the flat object list. Patch each layer's objectOffset to the
    // file offset of its first object.
    if (!objRows.empty())
    {
        const uint32_t objBlobOffset = static_cast<uint32_t>(std::ftell(f));
        std::fwrite(objRows.data(), sizeof(DTilemapObject), objRows.size(), f);

        // Patch layer rows.
        std::fseek(f, static_cast<long>(hdr.objectLayerOffset), SEEK_SET);
        for (auto& row : olRows)
        {
            const uint32_t firstIdx = row.objectOffset;
            row.objectOffset = objBlobOffset + firstIdx * sizeof(DTilemapObject);
        }
        std::fwrite(olRows.data(), sizeof(DObjectLayer), olRows.size(), f);
        std::fseek(f, 0, SEEK_END);
    }

    // Trailing pools (polygon points, properties, strings) — written contiguously
    // so Tilemap::Load can slurp them as a single tail blob keyed by string-pool
    // base.
    AppendBlob(f, polyPoints.data(), polyPoints.size());
    AppendBlob(f, propRows.data(),   propRows.size());
    AppendBlob(f, stringPool.data(), stringPool.size());

    // Rewrite header with patched offsets/counts.
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&hdr, sizeof(hdr), 1, f);

    std::fclose(f);
    return true;
}

} // namespace DekiTilemap

#endif // DEKI_EDITOR
