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
    : m_MFs(fs)
    , m_MHeader(header)
    , m_MIndex(index)
    , m_indexCount(indexCount)
{
    m_chunkBytes = static_cast<size_t>(header.chunkWidth) *
                   static_cast<size_t>(header.chunkHeight) * sizeof(uint32_t);

    m_MHandle = m_MFs->OpenFile(dtilemapPath, IDekiFileSystem::OpenMode::READ_BINARY);
    if (!m_MHandle)
        DEKI_LOG_ERROR("TilemapStreamer: cannot open '%s' for streaming", dtilemapPath);
}

TilemapStreamer::~TilemapStreamer()
{
    for (auto& [key, rc] : m_MResident)
        std::free(rc.owned);
    if (m_MHandle && m_MFs)
        m_MFs->CloseFile(m_MHandle);
}

void TilemapStreamer::SetMemoryBudget(size_t bytes)
{
    m_budgetBytes = bytes;
    EvictUntilUnder(m_budgetBytes);
}

const ChunkIndexEntry* TilemapStreamer::FindIndexEntry(int32_t layerIdx, int32_t cx, int32_t cy) const
{
    const uint16_t layer16 = static_cast<uint16_t>(layerIdx);
    const auto* begin = m_MIndex;
    const auto* end   = m_MIndex + m_indexCount;

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

    // m_MIndex is sorted by (layerIndex, chunkY, chunkX) at load time
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
        if (m_MResident.find(key) != m_MResident.end())
            continue;
        const ChunkIndexEntry* e = FindIndexEntry(layerIdx, cx, cy);
        if (!e)
            continue;   // index says nothing here — treat as empty
        if (m_pendingSet.insert(key).second)
            m_MPending.push_back(key);
    }
}

bool TilemapStreamer::LoadChunkNow(const ChunkIndexEntry& entry)
{
    Key key{entry.chunkX, entry.chunkY, entry.layerIndex};
    if (m_MResident.find(key) != m_MResident.end())
        return true;

    if (m_residentBytes + m_chunkBytes > m_budgetBytes)
        EvictUntilUnder(m_budgetBytes > m_chunkBytes ? m_budgetBytes - m_chunkBytes : 0);

    ResidentChunk rc{};
    rc.chunk.chunkX     = entry.chunkX;
    rc.chunk.chunkY     = entry.chunkY;
    rc.chunk.layerIndex = entry.layerIndex;
    rc.chunk.width      = m_MHeader.chunkWidth;
    rc.chunk.height     = m_MHeader.chunkHeight;
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
        if (m_MHandle)
        {
            m_MFs->SeekFile(m_MHandle, static_cast<long>(entry.payloadOffset),
                           IDekiFileSystem::SeekOrigin::BEGIN);
            m_MFs->ReadFile(m_MHandle, &fill, sizeof(fill));
        }
        const size_t n = static_cast<size_t>(rc.chunk.width) * rc.chunk.height;
        for (size_t i = 0; i < n; ++i) rc.owned[i] = fill;
    }
    else
    {
        if (!m_MHandle)
        {
            std::free(rc.owned);
            return false;
        }
        m_MFs->SeekFile(m_MHandle, static_cast<long>(entry.payloadOffset),
                       IDekiFileSystem::SeekOrigin::BEGIN);
        size_t got = m_MFs->ReadFile(m_MHandle, rc.owned, m_chunkBytes);
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
    m_MLru.push_back(key);
    rc.lruIt = std::prev(m_MLru.end());

    m_residentBytes += rc.bytes;
    m_MResident.emplace(key, std::move(rc));
    return true;
}

void TilemapStreamer::Pump(size_t byteBudget)
{
    size_t spent = 0;
    while (!m_MPending.empty() && spent < byteBudget)
    {
        Key k = m_MPending.front();
        m_MPending.pop_front();
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
    auto it = m_MResident.find(k);
    if (it == m_MResident.end()) return nullptr;
    return &it->second.chunk;
}

const TileChunk* TilemapStreamer::GetAndTouch(int32_t layerIdx, int32_t chunkX, int32_t chunkY, uint32_t frame)
{
    Key k{chunkX, chunkY, static_cast<uint16_t>(layerIdx)};
    auto it = m_MResident.find(k);
    if (it == m_MResident.end()) return nullptr;
    ResidentChunk& rc = it->second;
    if (rc.lastTouchFrame != frame)
    {
        rc.lastTouchFrame = frame;
        if (rc.lruIt != std::prev(m_MLru.end()))
        {
            m_MLru.splice(m_MLru.end(), m_MLru, rc.lruIt);  // move the node, no alloc
            rc.lruIt = std::prev(m_MLru.end());
        }
    }
    return &rc.chunk;
}

void TilemapStreamer::TouchLRU(int32_t layerIdx, int32_t chunkX, int32_t chunkY)
{
    Key k{chunkX, chunkY, static_cast<uint16_t>(layerIdx)};
    auto it = m_MResident.find(k);
    if (it == m_MResident.end()) return;
    m_MLru.erase(it->second.lruIt);
    m_MLru.push_back(k);
    it->second.lruIt = std::prev(m_MLru.end());
}

void TilemapStreamer::EvictUntilUnder(size_t targetBytes)
{
    while (m_residentBytes > targetBytes && !m_MLru.empty())
    {
        Key oldest = m_MLru.front();
        m_MLru.pop_front();
        auto it = m_MResident.find(oldest);
        if (it == m_MResident.end()) continue;
        std::free(it->second.owned);
        m_residentBytes -= it->second.bytes;
        m_MResident.erase(it);
    }
}

} // namespace DekiTilemap
