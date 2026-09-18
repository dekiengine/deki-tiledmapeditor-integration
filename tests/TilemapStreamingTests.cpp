// Chunk streaming: what actually gets read off storage while a map scrolls,
// and what is thrown away when the cache is full.
//
// The streamer keeps decoded chunks in memory up to a budget and drops the
// least recently drawn ones past it. That budget had no caller until now —
// every target ran on the 256 KiB default chosen for an ESP32 — so the eviction
// path had never been exercised by anything.
//
// The fixtures build a real .dtilemap in memory, payloads included, and serve
// it through a filesystem mounted at "S:/".

#include <gtest/gtest.h>

#include "TileChunk.h"
#include "Tilemap.h"
#include "TilemapStreamer.h"

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>
#include <deki/providers/Memory.h>
#include <deki/platforms/desktop/DesktopMemoryProvider.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using DekiTiledMap::ChunkIndexEntry;
using DekiTiledMap::DTilemapHeader;
using DekiTiledMap::TileChunk;
using DekiTiledMap::Tilemap;
using DekiTiledMap::TilemapStreamer;

namespace
{
constexpr uint16_t kChunkW = 4;
constexpr uint16_t kChunkH = 4;
constexpr size_t kTilesPerChunk = kChunkW * kChunkH;
constexpr size_t kChunkBytes = kTilesPerChunk * sizeof(uint32_t);

class MemoryFileSystem : public Deki::IFileSystem
{
public:
    MemoryFileSystem(std::string path, std::vector<uint8_t> bytes)
        : m_Path(std::move(path)), m_Bytes(std::move(bytes))
    {
    }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    FileHandle OpenFile(const char* path, OpenMode mode) override
    {
        if (mode != OpenMode::READ_BINARY || !path || m_Path != path) return nullptr;
        m_Cursor = 0;
        return reinterpret_cast<FileHandle>(this);
    }
    void CloseFile(FileHandle) override {}
    size_t ReadFile(FileHandle, void* buffer, size_t size) override
    {
        if (m_Cursor >= m_Bytes.size()) return 0;
        const size_t n = std::min(size, m_Bytes.size() - m_Cursor);
        std::memcpy(buffer, m_Bytes.data() + m_Cursor, n);
        m_Cursor += n;
        bytesRead += n;
        return n;
    }
    size_t WriteFile(FileHandle, const void*, size_t) override { return 0; }
    long SeekFile(FileHandle, long offset, SeekOrigin origin) override
    {
        long base = 0;
        if (origin == SeekOrigin::CURRENT) base = static_cast<long>(m_Cursor);
        else if (origin == SeekOrigin::END) base = static_cast<long>(m_Bytes.size());
        long t = base + offset;
        if (t < 0) t = 0;
        if (t > static_cast<long>(m_Bytes.size())) t = static_cast<long>(m_Bytes.size());
        m_Cursor = static_cast<size_t>(t);
        return t;
    }
    long TellFile(FileHandle) override { return static_cast<long>(m_Cursor); }
    long GetFileSize(FileHandle) override { return static_cast<long>(m_Bytes.size()); }
    bool FileExists(const char* path) override { return path && m_Path == path; }
    bool ConvertPath(const char* v, char* out, size_t cap) override
    {
        if (!v || !out) return false;
        std::snprintf(out, cap, "%s", v);
        return true;
    }

    size_t bytesRead = 0;

private:
    std::string m_Path;
    std::vector<uint8_t> m_Bytes;
    size_t m_Cursor = 0;
};

// A map whose chunks each carry a full payload: every tile of chunk (x, y) on
// layer L holds the gid `Gid(L, x, y)`, so a test can tell chunks apart by
// reading one tile.
uint32_t Gid(uint16_t layer, int32_t cx, int32_t cy)
{
    return 1u + static_cast<uint32_t>(layer) * 1000u +
           static_cast<uint32_t>(cy) * 10u + static_cast<uint32_t>(cx);
}

struct StreamingMap
{
    std::vector<uint8_t> bytes;
    DTilemapHeader header{};
    std::vector<ChunkIndexEntry> index;
};

// `gridW` x `gridH` chunks on one layer, all with real payloads.
StreamingMap BuildMap(int32_t gridW, int32_t gridH, uint16_t layer = 0)
{
    StreamingMap m;
    std::memcpy(m.header.magic, "DTM1", 4);
    m.header.version = 1;
    m.header.mapWidth = static_cast<uint32_t>(gridW) * kChunkW;
    m.header.mapHeight = static_cast<uint32_t>(gridH) * kChunkH;
    m.header.tileWidth = 16;
    m.header.tileHeight = 16;
    m.header.chunkWidth = kChunkW;
    m.header.chunkHeight = kChunkH;
    m.header.layerCount = static_cast<uint32_t>(layer) + 1u;

    const uint32_t indexOffset = sizeof(DTilemapHeader);
    const uint32_t count = static_cast<uint32_t>(gridW * gridH);
    uint32_t payloadCursor = indexOffset + count * sizeof(ChunkIndexEntry);

    for (int32_t y = 0; y < gridH; ++y)
    {
        for (int32_t x = 0; x < gridW; ++x)
        {
            ChunkIndexEntry e{};
            e.layerIndex = layer;
            e.chunkX = x;
            e.chunkY = y;
            e.payloadOffset = payloadCursor;
            payloadCursor += static_cast<uint32_t>(kChunkBytes);
            m.index.push_back(e);
        }
    }

    m.header.chunkIndexOffset = indexOffset;
    m.header.chunkIndexCount = count;
    m.header.tilesetTableOffset = payloadCursor;
    m.header.tilesetCount = 0;
    m.header.objectLayerOffset = payloadCursor;
    m.header.objectLayerCount = 0;

    m.bytes.resize(payloadCursor);
    std::memcpy(m.bytes.data(), &m.header, sizeof(m.header));
    std::memcpy(m.bytes.data() + indexOffset, m.index.data(),
                m.index.size() * sizeof(ChunkIndexEntry));
    for (const auto& e : m.index)
    {
        std::vector<uint32_t> tiles(kTilesPerChunk, Gid(e.layerIndex, e.chunkX, e.chunkY));
        std::memcpy(m.bytes.data() + e.payloadOffset, tiles.data(), kChunkBytes);
    }
    return m;
}

class Streaming : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        // The streamer allocates chunk buffers through Deki::Memory.
        if (!Deki::Memory::IsInitialized())
        {
            Deki::Memory::SetBackend(new Deki::DesktopMemoryProvider());
            Deki::Memory::Initialize();
        }
    }

    TilemapStreamer* Open(const StreamingMap& m)
    {
        m_Map = m;
        m_Fs = std::make_unique<MemoryFileSystem>("S:/s.dtilemap", m_Map.bytes);
        m_Streamer = new TilemapStreamer(m_Fs.get(), "S:/s.dtilemap", m_Map.header,
                                         m_Map.index.data(), m_Map.index.size());
        return m_Streamer;
    }
    void TearDown() override
    {
        delete m_Streamer;
        m_Streamer = nullptr;
        m_Fs.reset();
    }
    MemoryFileSystem& Fs() { return *m_Fs; }

private:
    StreamingMap m_Map;
    std::unique_ptr<MemoryFileSystem> m_Fs;
    TilemapStreamer* m_Streamer = nullptr;
};

// Enough budget for `n` chunks and not one more.
size_t BudgetFor(int n) { return kChunkBytes * static_cast<size_t>(n); }
}  // namespace

TEST_F(Streaming, NothingIsResidentUntilPumped)
{
    TilemapStreamer* s = Open(BuildMap(2, 2));
    s->RequestRect(0, 0, 0, 1, 1);

    EXPECT_EQ(s->Get(0, 0, 0), nullptr) << "RequestRect queues, it does not read";
    EXPECT_EQ(s->ResidentBytes(), 0u);
}

TEST_F(Streaming, PumpingLoadsTheRequestedChunks)
{
    TilemapStreamer* s = Open(BuildMap(2, 2));
    s->SetMemoryBudget(BudgetFor(16));
    s->RequestRect(0, 0, 0, 1, 1);
    s->Pump(BudgetFor(16));

    const TileChunk* c = s->Get(0, 1, 1);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->chunkX, 1);
    EXPECT_EQ(c->chunkY, 1);
    EXPECT_EQ(c->width, kChunkW);
    ASSERT_NE(c->tileGids, nullptr);
    EXPECT_EQ(c->tileGids[0], Gid(0, 1, 1)) << "the payload read belongs to this chunk";
    EXPECT_EQ(c->tileGids[kTilesPerChunk - 1], Gid(0, 1, 1));
}

TEST_F(Streaming, AChunkOutsideTheIndexIsNeverResident)
{
    TilemapStreamer* s = Open(BuildMap(2, 2));
    s->SetMemoryBudget(BudgetFor(16));
    s->RequestRect(0, 0, 0, 8, 8);  // asks well past the edge
    s->Pump(BudgetFor(16));

    EXPECT_EQ(s->Get(0, 5, 5), nullptr);
    EXPECT_LE(s->ResidentBytes(), BudgetFor(4)) << "only the four real chunks exist";
}

TEST_F(Streaming, AByteBudgetLimitsOneCallNotTheWholeQueue)
{
    TilemapStreamer* s = Open(BuildMap(4, 4));
    s->SetMemoryBudget(BudgetFor(64));
    s->RequestRect(0, 0, 0, 3, 3);  // 16 chunks queued

    s->Pump(kChunkBytes);  // room for one
    const size_t afterFirst = s->ResidentBytes();
    EXPECT_GT(afterFirst, 0u);
    EXPECT_LE(afterFirst, kChunkBytes * 2) << "a byte budget must bound the IO per call";

    for (int i = 0; i < 32; ++i) s->Pump(kChunkBytes);
    EXPECT_GT(s->ResidentBytes(), afterFirst) << "later calls drain the rest of the queue";
}

TEST_F(Streaming, ResidentBytesStaysUnderTheBudget)
{
    TilemapStreamer* s = Open(BuildMap(4, 4));
    s->SetMemoryBudget(BudgetFor(4));
    s->RequestRect(0, 0, 0, 3, 3);
    for (int i = 0; i < 32; ++i) s->Pump(BudgetFor(16));

    EXPECT_LE(s->ResidentBytes(), BudgetFor(4))
        << "16 chunks were asked for with room for 4";
    EXPECT_GT(s->ResidentBytes(), 0u) << "something should be cached";
}

TEST_F(Streaming, LoweringTheBudgetEvictsImmediately)
{
    TilemapStreamer* s = Open(BuildMap(4, 4));
    s->SetMemoryBudget(BudgetFor(16));
    s->RequestRect(0, 0, 0, 3, 3);
    for (int i = 0; i < 32; ++i) s->Pump(BudgetFor(16));
    ASSERT_GT(s->ResidentBytes(), BudgetFor(2));

    s->SetMemoryBudget(BudgetFor(2));
    EXPECT_LE(s->ResidentBytes(), BudgetFor(2))
        << "a smaller budget must take effect at once, not at the next load";
}

TEST_F(Streaming, RaisingTheBudgetKeepsWhatIsAlreadyThere)
{
    TilemapStreamer* s = Open(BuildMap(4, 4));
    s->SetMemoryBudget(BudgetFor(4));
    s->RequestRect(0, 0, 0, 3, 3);
    for (int i = 0; i < 32; ++i) s->Pump(BudgetFor(16));
    const size_t before = s->ResidentBytes();

    s->SetMemoryBudget(BudgetFor(16));
    EXPECT_EQ(s->ResidentBytes(), before) << "raising the ceiling evicts nothing";
    EXPECT_EQ(s->MemoryBudget(), BudgetFor(16));
}

TEST_F(Streaming, TheChunkDrawnMostRecentlySurvivesEviction)
{
    TilemapStreamer* s = Open(BuildMap(4, 1));  // four chunks in a row
    s->SetMemoryBudget(BudgetFor(4));
    s->RequestRect(0, 0, 0, 3, 0);
    for (int i = 0; i < 16; ++i) s->Pump(BudgetFor(8));
    ASSERT_NE(s->Get(0, 0, 0), nullptr);

    // Draw chunk 0 again, making chunk 1 the oldest, then squeeze the cache.
    s->TouchLRU(0, 0, 0);
    s->SetMemoryBudget(BudgetFor(1));

    EXPECT_NE(s->Get(0, 0, 0), nullptr) << "the most recently drawn chunk must be the one kept";
}

TEST_F(Streaming, AResidentChunkIsNotReadTwice)
{
    TilemapStreamer* s = Open(BuildMap(2, 2));
    s->SetMemoryBudget(BudgetFor(16));
    s->RequestRect(0, 0, 0, 1, 1);
    for (int i = 0; i < 8; ++i) s->Pump(BudgetFor(16));
    const size_t afterLoad = Fs().bytesRead;
    ASSERT_GT(afterLoad, 0u);

    s->RequestRect(0, 0, 0, 1, 1);
    for (int i = 0; i < 8; ++i) s->Pump(BudgetFor(16));

    EXPECT_EQ(Fs().bytesRead, afterLoad) << "asking again for a cached chunk must not re-read it";
}

TEST_F(Streaming, GetAndTouchReturnsTheSameChunkAsGet)
{
    TilemapStreamer* s = Open(BuildMap(2, 2));
    s->SetMemoryBudget(BudgetFor(16));
    s->RequestRect(0, 0, 0, 1, 1);
    for (int i = 0; i < 8; ++i) s->Pump(BudgetFor(16));

    const TileChunk* a = s->Get(0, 1, 0);
    const TileChunk* b = s->GetAndTouch(0, 1, 0, 1);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);
    EXPECT_EQ(b->tileGids[0], Gid(0, 1, 0));
}

TEST_F(Streaming, TheChunkSizeComesFromTheHeader)
{
    TilemapStreamer* s = Open(BuildMap(1, 1));
    EXPECT_EQ(s->ChunkWidth(), kChunkW);
    EXPECT_EQ(s->ChunkHeight(), kChunkH);
}
