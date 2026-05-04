#include "TilemapStreamer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "DekiLogSystem.h"

namespace DekiTilemap
{

TilemapStreamer::TilemapStreamer(IDekiFileSystem* fs,
                                 const char* dtilemapPath,
                                 const DTilemapHeader& header,
                                 const ChunkIndexEntry* index,
                                 size_t indexCount)
    : m_fs(fs)
    , m_header(header)
    , m_index(index)
    , m_indexCount(indexCount)
{
    m_chunkBytes = static_cast<size_t>(header.chunkWidth) *
                   static_cast<size_t>(header.chunkHeight) * sizeof(uint32_t);

    m_handle = m_fs->OpenFile(dtilemapPath, IDekiFileSystem::OpenMode::READ_BINARY);
    if (!m_handle)
        DEKI_LOG_ERROR("TilemapStreamer: cannot open '%s' for streaming", dtilemapPath);
}

TilemapStreamer::~TilemapStreamer()
{
    for (auto& [key, rc] : m_resident)
        std::free(rc.owned);
    if (m_handle && m_fs)
        m_fs->CloseFile(m_handle);
}

void TilemapStreamer::SetMemoryBudget(size_t bytes)
{
    m_budgetBytes = bytes;
    EvictUntilUnder(m_budgetBytes);
}

const ChunkIndexEntry* TilemapStreamer::FindIndexEntry(int32_t layerIdx, int32_t cx, int32_t cy) const
{
    const uint16_t layer16 = static_cast<uint16_t>(layerIdx);
    const auto* begin = m_index;
    const auto* end   = m_index + m_indexCount;

    // Spatial-locality cache: RequestRect scans in (cy, cx) order, so the next
    // probe usually wants the entry right after the previous hit. Check the
    // cached pointer and its successor before paying for a fresh binary search.
    if (m_lastFound && m_lastFound >= begin && m_lastFound < end)
    {
        if (m_lastFound->layerIndex == layer16 &&
            m_lastFound->chunkY == cy && m_lastFound->chunkX == cx)
            return m_lastFound;
        const auto* nxt = m_lastFound + 1;
        if (nxt < end &&
            nxt->layerIndex == layer16 &&
            nxt->chunkY == cy && nxt->chunkX == cx)
        {
            m_lastFound = nxt;
            return nxt;
        }
    }

    // m_index is sorted by (layerIndex, chunkY, chunkX) at load time
    // (Tilemap::Load), so binary search lands on the exact entry.
    ChunkIndexEntry key{};
    key.chunkX     = cx;
    key.chunkY     = cy;
    key.layerIndex = layer16;
    auto cmp = [](const ChunkIndexEntry& a, const ChunkIndexEntry& b)
    {
        if (a.layerIndex != b.layerIndex) return a.layerIndex < b.layerIndex;
        if (a.chunkY     != b.chunkY)     return a.chunkY     < b.chunkY;
        return a.chunkX < b.chunkX;
    };
    const auto* it = std::lower_bound(begin, end, key, cmp);
    if (it == end) return nullptr;
    if (it->layerIndex != key.layerIndex || it->chunkY != key.chunkY || it->chunkX != key.chunkX)
        return nullptr;
    m_lastFound = it;
    return it;
}

void TilemapStreamer::RequestRect(int32_t layerIdx,
                                  int32_t chunkMinX, int32_t chunkMinY,
                                  int32_t chunkMaxX, int32_t chunkMaxY)
{
    for (int32_t cy = chunkMinY; cy <= chunkMaxY; ++cy)
    for (int32_t cx = chunkMinX; cx <= chunkMaxX; ++cx)
    {
        Key key{cx, cy, static_cast<uint16_t>(layerIdx)};
        if (m_resident.find(key) != m_resident.end())
            continue;
        const ChunkIndexEntry* e = FindIndexEntry(layerIdx, cx, cy);
        if (!e)
            continue;   // index says nothing here — treat as empty
        if (m_pendingSet.insert(key).second)
            m_pending.push_back(key);
    }
}

bool TilemapStreamer::LoadChunkNow(const ChunkIndexEntry& entry)
{
    Key key{entry.chunkX, entry.chunkY, entry.layerIndex};
    if (m_resident.find(key) != m_resident.end())
        return true;

    if (m_residentBytes + m_chunkBytes > m_budgetBytes)
        EvictUntilUnder(m_budgetBytes > m_chunkBytes ? m_budgetBytes - m_chunkBytes : 0);

    ResidentChunk rc{};
    rc.chunk.chunkX     = entry.chunkX;
    rc.chunk.chunkY     = entry.chunkY;
    rc.chunk.layerIndex = entry.layerIndex;
    rc.chunk.width      = m_header.chunkWidth;
    rc.chunk.height     = m_header.chunkHeight;
    rc.chunk.flags      = entry.flags;
    rc.bytes            = m_chunkBytes;
    rc.owned            = static_cast<uint32_t*>(std::malloc(m_chunkBytes));
    if (!rc.owned)
    {
        DEKI_LOG_ERROR("TilemapStreamer: alloc failed for chunk (%d,%d) layer %u",
                       entry.chunkX, entry.chunkY, static_cast<unsigned>(entry.layerIndex));
        return false;
    }

    if (entry.flags & CHUNK_FLAG_EMPTY)
    {
        std::memset(rc.owned, 0, m_chunkBytes);
    }
    else if (entry.flags & CHUNK_FLAG_UNIFORM_FILL)
    {
        uint32_t fill = 0;
        if (m_handle)
        {
            m_fs->SeekFile(m_handle, static_cast<long>(entry.payloadOffset),
                           IDekiFileSystem::SeekOrigin::BEGIN);
            m_fs->ReadFile(m_handle, &fill, sizeof(fill));
        }
        const size_t n = static_cast<size_t>(rc.chunk.width) * rc.chunk.height;
        for (size_t i = 0; i < n; ++i) rc.owned[i] = fill;
    }
    else
    {
        if (!m_handle)
        {
            std::free(rc.owned);
            return false;
        }
        m_fs->SeekFile(m_handle, static_cast<long>(entry.payloadOffset),
                       IDekiFileSystem::SeekOrigin::BEGIN);
        size_t got = m_fs->ReadFile(m_handle, rc.owned, m_chunkBytes);
        if (got != m_chunkBytes)
        {
            DEKI_LOG_ERROR("TilemapStreamer: short read on chunk (%d,%d) layer %u: got %zu of %zu",
                           entry.chunkX, entry.chunkY, static_cast<unsigned>(entry.layerIndex),
                           got, m_chunkBytes);
            std::free(rc.owned);
            return false;
        }
    }

    rc.chunk.tileGids = rc.owned;
    m_lru.push_back(key);
    rc.lruIt = std::prev(m_lru.end());

    m_residentBytes += rc.bytes;
    m_resident.emplace(key, std::move(rc));
    return true;
}

void TilemapStreamer::Pump(size_t byteBudget)
{
    size_t spent = 0;
    while (!m_pending.empty() && spent < byteBudget)
    {
        Key k = m_pending.front();
        m_pending.pop_front();
        m_pendingSet.erase(k);
        const ChunkIndexEntry* e = FindIndexEntry(static_cast<int32_t>(k.layer), k.cx, k.cy);
        if (!e) continue;
        if (LoadChunkNow(*e))
            spent += m_chunkBytes;
    }
}

const TileChunk* TilemapStreamer::Get(int32_t layerIdx, int32_t chunkX, int32_t chunkY)
{
    Key k{chunkX, chunkY, static_cast<uint16_t>(layerIdx)};
    auto it = m_resident.find(k);
    if (it == m_resident.end()) return nullptr;
    return &it->second.chunk;
}

void TilemapStreamer::TouchLRU(int32_t layerIdx, int32_t chunkX, int32_t chunkY)
{
    Key k{chunkX, chunkY, static_cast<uint16_t>(layerIdx)};
    auto it = m_resident.find(k);
    if (it == m_resident.end()) return;
    m_lru.erase(it->second.lruIt);
    m_lru.push_back(k);
    it->second.lruIt = std::prev(m_lru.end());
}

void TilemapStreamer::EvictUntilUnder(size_t targetBytes)
{
    while (m_residentBytes > targetBytes && !m_lru.empty())
    {
        Key oldest = m_lru.front();
        m_lru.pop_front();
        auto it = m_resident.find(oldest);
        if (it == m_resident.end()) continue;
        std::free(it->second.owned);
        m_residentBytes -= it->second.bytes;
        m_resident.erase(it);
    }
}

} // namespace DekiTilemap
