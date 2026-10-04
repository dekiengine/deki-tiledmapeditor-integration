#include "Tilemap.h"
#include "TilemapStreamer.h"

#include <algorithm>
#include <cstring>

#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>
#include <deki/providers/FileSystem.h>

namespace DekiTiledMap
{

Tilemap::~Tilemap()
{
    delete m_MStreamer;
}

Tilemap* Tilemap::Load(const char* dtilemapPath)
{
    if (!dtilemapPath)
    {
        return nullptr;
    }

    // Through the engine filesystem, never stdio. The path the asset manager
    // hands us carries the cache directory, and on a device and in the
    // simulator that is the "S:/" mount — a prefix only IFileSystem knows how
    // to resolve. This used to be std::fopen, so a tilemap loaded in the editor
    // (native cache path) and nowhere else, while the chunk streamer set up at
    // the bottom of this function had always read through the filesystem.
    Deki::IFileSystem* fs = Deki::FileSystem::GetFileSystemForPath(dtilemapPath);
    if (!fs)
    {
        DEKI_LOG_ERROR("Tilemap::Load: no filesystem provider available");
        return nullptr;
    }

    Deki::IFileSystem::FileHandle f = fs->OpenFile(dtilemapPath, Deki::IFileSystem::OpenMode::ReadBinary);
    if (!f)
    {
        DEKI_LOG_ERROR("Tilemap::Load: cannot open '%s'", dtilemapPath);
        return nullptr;
    }

    DTilemapHeader hdr{};
    if (fs->ReadFile(f, &hdr, sizeof(hdr)) != sizeof(hdr))
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tilemap::Load: short read on header for '%s'", dtilemapPath);
        return nullptr;
    }
    if (std::memcmp(hdr.magic, "DTM1", 4) != 0 || hdr.version != 1)
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tilemap::Load: bad magic/version in '%s'", dtilemapPath);
        return nullptr;
    }

    // Chunk sides come from the file too. Zero divided by zero in the
    // collider (a panic on Xtensa); a huge one wrapped the streamer's chunk
    // buffer size on 32 bits, and its fill loop wrote far past the buffer.
    if (hdr.chunkWidth == 0 || hdr.chunkHeight == 0 || hdr.chunkWidth > 1024 || hdr.chunkHeight > 1024)
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tilemap::Load: '%s' has chunks of %ux%u tiles; 1 to 1024 each are supported", dtilemapPath,
                       (unsigned)hdr.chunkWidth, (unsigned)hdr.chunkHeight);
        return nullptr;
    }

    // Every count and offset below comes from the file, so each table is
    // checked against the file's size first, in 64 bits. Unchecked, a corrupt
    // map overflowed the object table (the per-layer counts wrapped their
    // uint32 sum), and a huge count made resize() abort on the device.
    const long fileSizeL = fs->GetFileSize(f);
    const uint64_t fileBytes = fileSizeL > 0 ? static_cast<uint64_t>(fileSizeL) : 0;
    auto inFile = [fileBytes](uint64_t offset, uint64_t count, uint64_t elem)
    { return offset <= fileBytes && count <= (fileBytes - offset) / elem; };
    if (!inFile(hdr.chunkIndexOffset, hdr.chunkIndexCount, sizeof(ChunkIndexEntry)) ||
        !inFile(hdr.tilesetTableOffset, hdr.tilesetCount, sizeof(TilesetRef)) ||
        !inFile(hdr.objectLayerOffset, hdr.objectLayerCount, sizeof(DObjectLayer)))
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tilemap::Load: '%s' is damaged (a table runs past the end of the file)", dtilemapPath);
        return nullptr;
    }

    auto* tm = new Tilemap();
    tm->m_MHeader = hdr;
    tm->m_AbsolutePath = dtilemapPath;
    auto fail = [&](const char* what) -> Tilemap*
    {
        fs->CloseFile(f);
        delete tm;
        DEKI_LOG_ERROR("Tilemap::Load: '%s' is damaged (%s)", dtilemapPath, what);
        return nullptr;
    };
    auto readAt = [&](uint64_t offset, void* into, uint64_t bytes)
    {
        fs->SeekFile(f, static_cast<long>(offset), Deki::IFileSystem::SeekOrigin::Begin);
        return fs->ReadFile(f, into, static_cast<size_t>(bytes)) == static_cast<size_t>(bytes);
    };

    if (hdr.chunkIndexCount > 0)
    {
        tm->m_MIndex.resize(hdr.chunkIndexCount);
        if (!readAt(hdr.chunkIndexOffset, tm->m_MIndex.data(), uint64_t(sizeof(ChunkIndexEntry)) * hdr.chunkIndexCount))
        {
            return fail("short chunk index");
        }

        // Sort by (layerIndex, chunkY, chunkX) so streamer + query paths can
        // do O(log N) binary search. Idempotent for already-sorted bakes.
        std::sort(tm->m_MIndex.begin(), tm->m_MIndex.end(),
                  [](const ChunkIndexEntry& a, const ChunkIndexEntry& b)
                  {
                      if (a.layerIndex != b.layerIndex)
                      {
                          return a.layerIndex < b.layerIndex;
                      }
                      if (a.chunkY != b.chunkY)
                      {
                          return a.chunkY < b.chunkY;
                      }
                      return a.chunkX < b.chunkX;
                  });
    }

    if (hdr.tilesetCount > 0)
    {
        tm->m_MTilesets.resize(hdr.tilesetCount);
        if (!readAt(hdr.tilesetTableOffset, tm->m_MTilesets.data(), uint64_t(sizeof(TilesetRef)) * hdr.tilesetCount))
        {
            return fail("short tileset table");
        }

        // Sort by firstGid so ResolveTilesetWithIndex can binary-search the
        // hot-path lookup. The baker conventionally writes ascending, but
        // sorting here makes the invariant explicit.
        std::sort(tm->m_MTilesets.begin(), tm->m_MTilesets.end(),
                  [](const TilesetRef& a, const TilesetRef& b) { return a.firstGid < b.firstGid; });
    }

    if (hdr.objectLayerCount > 0)
    {
        tm->m_ObjectLayers.resize(hdr.objectLayerCount);
        if (!readAt(hdr.objectLayerOffset, tm->m_ObjectLayers.data(),
                    uint64_t(sizeof(DObjectLayer)) * hdr.objectLayerCount))
        {
            return fail("short object layer table");
        }

        // Walk every layer and pull its object range. The baker writes
        // contiguous object blobs but we don't assume contiguity here � each
        // layer carries its own offset.
        uint64_t total = 0;
        for (const auto& l : tm->m_ObjectLayers)
        {
            if (!inFile(l.objectOffset, l.objectCount, sizeof(DTilemapObject)))
            {
                return fail("an object layer runs past the end of the file");
            }
            total += l.objectCount;
        }
        if (total > fileBytes / sizeof(DTilemapObject))
        {
            return fail("more objects than the file can hold");
        }
        if (total > 0)
        {
            tm->m_MObjects.resize(static_cast<size_t>(total));
            size_t cursor = 0;
            for (const auto& l : tm->m_ObjectLayers)
            {
                if (l.objectCount == 0)
                {
                    continue;
                }
                if (!readAt(l.objectOffset, tm->m_MObjects.data() + cursor,
                            uint64_t(sizeof(DTilemapObject)) * l.objectCount))
                {
                    return fail("short object table");
                }
                cursor += l.objectCount;
            }
        }
    }

    // The per-object pools: polygon points, properties, and the string pool
    // their names and values point into. They were never loaded, so every
    // object property (scene_guid for the spawner among them) read as absent.
    // A damaged pool is dropped rather than failing the map: the tiles do not
    // need it.
    uint64_t pointOffset = 0, pointCount = 0, propOffset = 0, propCount = 0, stringOffset = fileBytes;
    if (hdr.flags & kTilemapHasPools)
    {
        pointOffset = hdr.pointPoolOffset;
        pointCount = hdr.pointPoolCount;
        propOffset = hdr.propertyTableOffset;
        propCount = hdr.propertyCount;
        stringOffset = hdr.stringPoolOffset;
    }
    else if (!tm->m_MObjects.empty())
    {
        // Baked before the header named them. The baker wrote the object
        // list, then the points (always none), the properties, and the
        // strings last; an object's propertyOffset is its index among them.
        uint64_t objectsEnd = 0;
        for (const auto& l : tm->m_ObjectLayers)
        {
            objectsEnd =
                std::max(objectsEnd, uint64_t(l.objectOffset) + uint64_t(l.objectCount) * sizeof(DTilemapObject));
        }
        for (const auto& o : tm->m_MObjects)
        {
            propCount = std::max(propCount, uint64_t(o.propertyOffset) + o.propertyCount);
        }
        propOffset = objectsEnd;
        stringOffset = propCount > 0 && inFile(propOffset, propCount, sizeof(DTilemapProperty))
                           ? propOffset + propCount * sizeof(DTilemapProperty)
                           : fileBytes;
    }

    if (pointCount > 0 && inFile(pointOffset, pointCount, 2 * sizeof(int32_t)))
    {
        tm->m_PolygonPoints.resize(static_cast<size_t>(pointCount * 2));
        if (!readAt(pointOffset, tm->m_PolygonPoints.data(), pointCount * 2 * sizeof(int32_t)))
        {
            tm->m_PolygonPoints.clear();
        }
    }
    if (propCount > 0 && inFile(propOffset, propCount, sizeof(DTilemapProperty)))
    {
        tm->m_MProperties.resize(static_cast<size_t>(propCount));
        if (!readAt(propOffset, tm->m_MProperties.data(), propCount * sizeof(DTilemapProperty)))
        {
            tm->m_MProperties.clear();
        }
    }
    if (!tm->m_MProperties.empty() && stringOffset < fileBytes)
    {
        const uint64_t stringBytes = fileBytes - stringOffset;
        tm->m_StringPool.resize(static_cast<size_t>(stringBytes));
        if (!readAt(stringOffset, tm->m_StringPool.data(), stringBytes))
        {
            tm->m_StringPool.clear();
        }
    }
    if (propCount > 0 && tm->m_MProperties.size() < propCount)
    {
        DEKI_LOG_WARNING("Tilemap::Load: '%s' has damaged object properties; the objects load without them",
                         dtilemapPath);
    }

    fs->CloseFile(f);

    // The streamer keeps its own handle for chunk reads, on the same
    // filesystem this function read the header with.
    tm->m_MStreamer = new TilemapStreamer(fs, dtilemapPath, tm->m_MHeader, tm->m_MIndex.data(), tm->m_MIndex.size());

    // Both are pure functions of the data just loaded; the render pass reads
    // them every frame, so resolve them once here.
    tm->m_HasOrigin = tm->ComputeOrigin(tm->m_OriginX, tm->m_OriginY);
    tm->m_HasBounds = tm->ComputeAuthoredBounds(tm->m_BoundsMinX, tm->m_BoundsMinY, tm->m_BoundsW, tm->m_BoundsH);
    return tm;
}

const TilesetRef* Tilemap::ResolveTileset(uint32_t gid, uint32_t& outLocalId) const
{
    size_t unused = 0;
    return ResolveTilesetWithIndex(gid, outLocalId, unused);
}

const TilesetRef* Tilemap::ResolveTilesetWithIndex(uint32_t gid, uint32_t& outLocalId, size_t& outIndex) const
{
    const uint32_t idx = gid & kGidIndexMask;
    if (idx == 0)
    {
        return nullptr;
    }

    // m_MTilesets is sorted by firstGid (Load), so the matching entry is the
    // last one with firstGid <= idx — i.e. (upper_bound - 1).
    auto it = std::upper_bound(m_MTilesets.begin(), m_MTilesets.end(), idx,
                               [](uint32_t v, const TilesetRef& t) { return v < t.firstGid; });
    if (it == m_MTilesets.begin())
    {
        return nullptr;
    }
    --it;
    outLocalId = idx - it->firstGid;
    outIndex = static_cast<size_t>(it - m_MTilesets.begin());
    return &(*it);
}

void Tilemap::QueryVisibleChunks(int32_t layerIdx, int32_t chunkMinX, int32_t chunkMinY, int32_t chunkMaxX,
                                 int32_t chunkMaxY, std::vector<ChunkIndexEntry>& out) const
{
    out.clear();
    if (m_MIndex.empty())
    {
        return;
    }

    // The index is sorted by (layerIndex, chunkY, chunkX). Bracket the
    // requested layer + Y range with two binary searches, then linear-walk
    // the (typically small) bracketed slice and filter by X.
    const auto cmpLess = [](const ChunkIndexEntry& e, std::pair<int32_t, int32_t> key)
    {
        if (static_cast<int32_t>(e.layerIndex) != key.first)
        {
            return static_cast<int32_t>(e.layerIndex) < key.first;
        }
        return e.chunkY < key.second;
    };

    auto lo = std::lower_bound(m_MIndex.begin(), m_MIndex.end(), std::make_pair(layerIdx, chunkMinY), cmpLess);
    auto hi = std::lower_bound(m_MIndex.begin(), m_MIndex.end(), std::make_pair(layerIdx, chunkMaxY + 1), cmpLess);

    for (auto it = lo; it != hi; ++it)
    {
        if (it->chunkX < chunkMinX || it->chunkX > chunkMaxX)
        {
            continue;
        }
        out.push_back(*it);
    }
}

const DTilemapProperty* Tilemap::ObjectProperties(const DTilemapObject& obj, uint32_t& outCount) const
{
    outCount = 0;
    if (obj.propertyCount == 0 || uint64_t(obj.propertyOffset) + obj.propertyCount > m_MProperties.size())
    {
        return nullptr;
    }
    outCount = obj.propertyCount;
    return &m_MProperties[obj.propertyOffset];
}

std::string Tilemap::GetString(uint32_t offset) const
{
    // Offsets are from the start of the string pool, as the baker counts them.
    if (offset >= m_StringPool.size())
    {
        return {};
    }
    const char* p = m_StringPool.data() + offset;
    size_t maxLen = m_StringPool.size() - offset;
    size_t n = strnlen(p, maxLen);
    return std::string(p, n);
}

bool Tilemap::ComputeOrigin(float& outX, float& outY) const
{
    // DTilemapObject::name is an inlined char[32], not a string-pool offset,
    // so compare directly. strncmp is safe even if the field happens to fill
    // the full 32 bytes without a terminator.
    for (const auto& obj : m_MObjects)
    {
        if (std::strncmp(obj.name, "origin", sizeof(obj.name)) == 0)
        {
            outX = static_cast<float>(obj.x);
            outY = static_cast<float>(obj.y);
            return true;
        }
    }
    return false;
}

bool Tilemap::ComputeAuthoredBounds(int32_t& outMinTileX, int32_t& outMinTileY, int32_t& outWidthTiles,
                                    int32_t& outHeightTiles) const
{
    if (!IsInfinite())
    {
        outMinTileX = 0;
        outMinTileY = 0;
        outWidthTiles = static_cast<int32_t>(m_MHeader.mapWidth);
        outHeightTiles = static_cast<int32_t>(m_MHeader.mapHeight);
        return outWidthTiles > 0 && outHeightTiles > 0;
    }
    if (m_MIndex.empty())
    {
        return false;
    }

    int32_t minCx = m_MIndex[0].chunkX, maxCx = m_MIndex[0].chunkX;
    int32_t minCy = m_MIndex[0].chunkY, maxCy = m_MIndex[0].chunkY;
    for (const auto& e : m_MIndex)
    {
        if (e.chunkX < minCx)
        {
            minCx = e.chunkX;
        }
        if (e.chunkX > maxCx)
        {
            maxCx = e.chunkX;
        }
        if (e.chunkY < minCy)
        {
            minCy = e.chunkY;
        }
        if (e.chunkY > maxCy)
        {
            maxCy = e.chunkY;
        }
    }
    outMinTileX = minCx * static_cast<int32_t>(m_MHeader.chunkWidth);
    outMinTileY = minCy * static_cast<int32_t>(m_MHeader.chunkHeight);
    outWidthTiles = (maxCx - minCx + 1) * static_cast<int32_t>(m_MHeader.chunkWidth);
    outHeightTiles = (maxCy - minCy + 1) * static_cast<int32_t>(m_MHeader.chunkHeight);
    return true;
}

REGISTER_ASSET_TYPE(Tilemap, Tilemap::Load)

}  // namespace DekiTiledMap
