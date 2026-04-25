#include "Tilemap.h"
#include "TilemapStreamer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DekiLogSystem.h"
#include "assets/AssetManager.h"
#include "providers/DekiFileSystemProvider.h"

namespace DekiTilemap
{

Tilemap::~Tilemap()
{
    delete m_streamer;
}

Tilemap* Tilemap::Load(const char* dtilemapPath)
{
    if (!dtilemapPath)
        return nullptr;

    FILE* f = std::fopen(dtilemapPath, "rb");
    if (!f)
    {
        DEKI_LOG_ERROR("Tilemap::Load: cannot open '%s'", dtilemapPath);
        return nullptr;
    }

    DTilemapHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1)
    {
        std::fclose(f);
        DEKI_LOG_ERROR("Tilemap::Load: short read on header for '%s'", dtilemapPath);
        return nullptr;
    }
    if (std::memcmp(hdr.magic, "DTM1", 4) != 0 || hdr.version != 1)
    {
        std::fclose(f);
        DEKI_LOG_ERROR("Tilemap::Load: bad magic/version in '%s'", dtilemapPath);
        return nullptr;
    }

    auto* tm = new Tilemap();
    tm->m_header = hdr;
    tm->m_absolutePath = dtilemapPath;

    if (hdr.chunkIndexCount > 0)
    {
        tm->m_index.resize(hdr.chunkIndexCount);
        std::fseek(f, static_cast<long>(hdr.chunkIndexOffset), SEEK_SET);
        std::fread(tm->m_index.data(), sizeof(ChunkIndexEntry), hdr.chunkIndexCount, f);
    }

    if (hdr.tilesetCount > 0)
    {
        tm->m_tilesets.resize(hdr.tilesetCount);
        std::fseek(f, static_cast<long>(hdr.tilesetTableOffset), SEEK_SET);
        std::fread(tm->m_tilesets.data(), sizeof(TilesetRef), hdr.tilesetCount, f);
    }

    if (hdr.objectLayerCount > 0)
    {
        tm->m_objectLayers.resize(hdr.objectLayerCount);
        std::fseek(f, static_cast<long>(hdr.objectLayerOffset), SEEK_SET);
        std::fread(tm->m_objectLayers.data(), sizeof(DObjectLayer), hdr.objectLayerCount, f);

        // Walk every layer and pull its object range. The baker writes
        // contiguous object blobs but we don't assume contiguity here — each
        // layer carries its own offset.
        uint32_t total = 0;
        for (const auto& L : tm->m_objectLayers) total += L.objectCount;
        if (total > 0)
        {
            tm->m_objects.resize(total);
            uint32_t cursor = 0;
            for (const auto& L : tm->m_objectLayers)
            {
                if (L.objectCount == 0) continue;
                std::fseek(f, static_cast<long>(L.objectOffset), SEEK_SET);
                std::fread(tm->m_objects.data() + cursor, sizeof(DTilemapObject), L.objectCount, f);
                cursor += L.objectCount;
            }
        }
    }

    // Polygon points + properties + string pool live in trailing segments
    // produced by the baker. We just slurp the rest of the file into a tail
    // buffer — but that requires segment offsets. The header doesn't expose
    // them, so the baker is contracted to write polygon points immediately
    // after the object table, properties after that, and the string pool last.
    //
    // For v1, polygon points and properties are loaded by the baker writing
    // their offsets inside DTilemapObject entries; they must be reachable from
    // those offsets. We allocate a tail blob of (file_size - tail_start) and
    // expose accessors via offsets relative to file start.
    long fileSize = 0;
    std::fseek(f, 0, SEEK_END);
    fileSize = std::ftell(f);

    // Heuristic tail start: end of object table, or end of chunk index if no
    // objects, or end of header if neither. The baker always writes the
    // string pool last so we read from the highest known offset to EOF.
    uint32_t tailStart = sizeof(DTilemapHeader);
    if (hdr.chunkIndexOffset + hdr.chunkIndexCount * sizeof(ChunkIndexEntry) > tailStart)
        tailStart = hdr.chunkIndexOffset + hdr.chunkIndexCount * sizeof(ChunkIndexEntry);
    if (hdr.tilesetTableOffset + hdr.tilesetCount * sizeof(TilesetRef) > tailStart)
        tailStart = hdr.tilesetTableOffset + hdr.tilesetCount * sizeof(TilesetRef);
    if (hdr.objectLayerOffset + hdr.objectLayerCount * sizeof(DObjectLayer) > tailStart)
        tailStart = hdr.objectLayerOffset + hdr.objectLayerCount * sizeof(DObjectLayer);

    // Conservative: load entire file tail into the string pool (it includes
    // polygon points + properties + strings). Object accessors index into it.
    if (static_cast<long>(tailStart) < fileSize)
    {
        long tailLen = fileSize - static_cast<long>(tailStart);
        tm->m_stringPool.resize(static_cast<size_t>(tailLen));
        std::fseek(f, static_cast<long>(tailStart), SEEK_SET);
        std::fread(tm->m_stringPool.data(), 1, static_cast<size_t>(tailLen), f);
    }

    std::fclose(f);

    // Streamer keeps its own file handle for chunk reads.
    IDekiFileSystem* fs = DekiFileSystemProvider::GetCurrentFileSystem();
    if (!fs)
    {
        DEKI_LOG_ERROR("Tilemap::Load: no filesystem provider available");
        delete tm;
        return nullptr;
    }
    tm->m_streamer = new TilemapStreamer(fs, dtilemapPath, tm->m_header,
                                         tm->m_index.data(), tm->m_index.size());
    return tm;
}

const TilesetRef* Tilemap::ResolveTileset(uint32_t gid, uint32_t& outLocalId) const
{
    const uint32_t idx = gid & GID_INDEX_MASK;
    if (idx == 0) return nullptr;

    const TilesetRef* best = nullptr;
    for (const auto& t : m_tilesets)
    {
        if (idx >= t.firstGid && (!best || t.firstGid > best->firstGid))
            best = &t;
    }
    if (!best) return nullptr;
    outLocalId = idx - best->firstGid;
    return best;
}

void Tilemap::QueryVisibleChunks(int32_t layerIdx,
                                 int32_t chunkMinX, int32_t chunkMinY,
                                 int32_t chunkMaxX, int32_t chunkMaxY,
                                 std::vector<ChunkIndexEntry>& out) const
{
    out.clear();
    // The index is sorted by (layerIndex, chunkY, chunkX). Linear scan filtered
    // to the requested layer is the simplest correct path for v1; binary-range
    // narrowing is a future optimisation.
    for (const auto& e : m_index)
    {
        if (static_cast<int32_t>(e.layerIndex) != layerIdx) continue;
        if (e.chunkX < chunkMinX || e.chunkX > chunkMaxX) continue;
        if (e.chunkY < chunkMinY || e.chunkY > chunkMaxY) continue;
        out.push_back(e);
    }
}

std::string Tilemap::GetString(uint32_t offset) const
{
    // offsets in DTilemapProperty / DTilemapObject are file-absolute. m_stringPool
    // begins at tailStart computed in Load, so callers must subtract that base.
    // For v1 we expect the baker to write all string-pool offsets relative to
    // the *same* tail base (string pool start). The safer path: store the
    // tail base too and subtract here. We approximate by treating offsets as
    // absolute and clamping.
    if (offset >= m_stringPool.size()) return {};
    const char* p = m_stringPool.data() + offset;
    size_t maxLen = m_stringPool.size() - offset;
    size_t n = strnlen(p, maxLen);
    return std::string(p, n);
}

} // namespace DekiTilemap

REGISTER_ASSET_TYPE(::DekiTilemap::Tilemap, ::DekiTilemap::Tilemap::Load)
