// The map's query surface: resolving a global tile id to its tileset, and
// bracketing the chunk index by layer and rectangle.
//
// Both are binary searches over tables the loader sorts, and both run per frame
// while a map is drawn. Nothing exercised either until now: this package had no
// test target at all, so the first coverage it gets should be the two functions
// a wrong answer would show up in as tiles from the wrong sheet, or chunks that
// never appear.
//
// Fixtures are built in memory and served through a filesystem mounted at
// "S:/", the same way a device sees its SD card.

#include <gtest/gtest.h>

#include "TileChunk.h"  // GID_INDEX_MASK
#include "Tilemap.h"

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using DekiTiledMap::ChunkIndexEntry;
using DekiTiledMap::DObjectLayer;
using DekiTiledMap::DTilemapHeader;
using DekiTiledMap::Tilemap;
using DekiTiledMap::TilesetRef;

namespace
{
// Same one-file in-memory filesystem the loader tests use.
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

private:
    std::string m_Path;
    std::vector<uint8_t> m_Bytes;
    size_t m_Cursor = 0;
};

// Lays out a .dtilemap: header, then chunk index, tileset table and object
// layers in that order, each pointed at by the header.
struct MapBuilder
{
    std::vector<ChunkIndexEntry> chunks;
    std::vector<TilesetRef> tilesets;
    std::vector<DObjectLayer> objectLayers;

    void AddTileset(const char* guid, uint32_t firstGid)
    {
        TilesetRef t{};
        std::snprintf(t.guid, sizeof(t.guid), "%s", guid);
        t.firstGid = firstGid;
        tilesets.push_back(t);
    }
    void AddChunk(uint16_t layer, int32_t cx, int32_t cy)
    {
        ChunkIndexEntry e{};
        e.layerIndex = layer;
        e.chunkX = cx;
        e.chunkY = cy;
        chunks.push_back(e);
    }

    std::vector<uint8_t> Build() const
    {
        DTilemapHeader h{};
        std::memcpy(h.magic, "DTM1", 4);
        h.version = 1;
        h.mapWidth = 64;
        h.mapHeight = 64;
        h.tileWidth = 16;
        h.tileHeight = 16;
        h.chunkWidth = 8;
        h.chunkHeight = 8;
        h.layerCount = 2;

        uint32_t cursor = sizeof(DTilemapHeader);
        h.chunkIndexOffset = cursor;
        h.chunkIndexCount = static_cast<uint32_t>(chunks.size());
        cursor += static_cast<uint32_t>(chunks.size() * sizeof(ChunkIndexEntry));
        h.tilesetTableOffset = cursor;
        h.tilesetCount = static_cast<uint32_t>(tilesets.size());
        cursor += static_cast<uint32_t>(tilesets.size() * sizeof(TilesetRef));
        h.objectLayerOffset = cursor;
        h.objectLayerCount = static_cast<uint32_t>(objectLayers.size());
        cursor += static_cast<uint32_t>(objectLayers.size() * sizeof(DObjectLayer));

        std::vector<uint8_t> out(cursor);
        std::memcpy(out.data(), &h, sizeof(h));
        if (!chunks.empty())
            std::memcpy(out.data() + h.chunkIndexOffset, chunks.data(),
                        chunks.size() * sizeof(ChunkIndexEntry));
        if (!tilesets.empty())
            std::memcpy(out.data() + h.tilesetTableOffset, tilesets.data(),
                        tilesets.size() * sizeof(TilesetRef));
        if (!objectLayers.empty())
            std::memcpy(out.data() + h.objectLayerOffset, objectLayers.data(),
                        objectLayers.size() * sizeof(DObjectLayer));
        return out;
    }
};

class MapFixture : public ::testing::Test
{
protected:
    Tilemap* LoadFrom(const MapBuilder& b)
    {
        m_Fs = std::make_unique<MemoryFileSystem>("S:/m.dtilemap", b.Build());
        Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
        Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
        m_Map = Tilemap::Load("S:/m.dtilemap");
        return m_Map;
    }
    void TearDown() override
    {
        delete m_Map;
        m_Map = nullptr;
        Deki::FileSystem::UnregisterFileSystem("S:/");
        Deki::FileSystem::SetDefaultFileSystem(nullptr);
        m_Fs.reset();
    }

private:
    std::unique_ptr<MemoryFileSystem> m_Fs;
    Tilemap* m_Map = nullptr;
};
}  // namespace

// ---- Resolving a global tile id to its tileset -----------------------------

TEST_F(MapFixture, AGidResolvesToTheTilesetItFallsIn)
{
    MapBuilder b;
    b.AddTileset("aaaa", 1);
    b.AddTileset("bbbb", 100);
    b.AddTileset("cccc", 200);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    uint32_t local = 0;
    const TilesetRef* t = map->ResolveTileset(1, local);
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->guid, "aaaa");
    EXPECT_EQ(local, 0u) << "the first tile of a sheet is local id 0";

    t = map->ResolveTileset(150, local);
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->guid, "bbbb");
    EXPECT_EQ(local, 50u);
}

TEST_F(MapFixture, TheBoundaryGidBelongsToTheNewTileset)
{
    // The classic off-by-one: firstGid is inclusive, so 100 is the first tile
    // of the second sheet, not the last of the first.
    MapBuilder b;
    b.AddTileset("aaaa", 1);
    b.AddTileset("bbbb", 100);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    uint32_t local = 0;
    const TilesetRef* t = map->ResolveTileset(99, local);
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->guid, "aaaa");
    EXPECT_EQ(local, 98u);

    t = map->ResolveTileset(100, local);
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->guid, "bbbb");
    EXPECT_EQ(local, 0u);
}

TEST_F(MapFixture, GidZeroIsTheEmptyTile)
{
    MapBuilder b;
    b.AddTileset("aaaa", 1);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    uint32_t local = 123;
    EXPECT_EQ(map->ResolveTileset(0, local), nullptr) << "gid 0 means no tile";
}

TEST_F(MapFixture, AGidBelowEveryTilesetResolvesToNothing)
{
    MapBuilder b;
    b.AddTileset("aaaa", 50);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    uint32_t local = 0;
    EXPECT_EQ(map->ResolveTileset(49, local), nullptr);
    EXPECT_NE(map->ResolveTileset(50, local), nullptr);
}

TEST_F(MapFixture, FlipFlagsAreStrippedBeforeResolving)
{
    // Tiled packs horizontal/vertical/diagonal flip into the top bits. They are
    // not part of the tile index, and a resolver that forgot to mask would send
    // every flipped tile off the end of the table.
    MapBuilder b;
    b.AddTileset("aaaa", 1);
    b.AddTileset("bbbb", 100);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    const uint32_t flipped = 150u | ~DekiTiledMap::GID_INDEX_MASK;

    uint32_t local = 0;
    const TilesetRef* t = map->ResolveTileset(flipped, local);
    ASSERT_NE(t, nullptr) << "a flipped tile must resolve like its unflipped twin";
    EXPECT_STREQ(t->guid, "bbbb");
    EXPECT_EQ(local, 50u);
}

TEST_F(MapFixture, TilesetsAreSortedEvenIfTheFileIsNot)
{
    // Load sorts the table so the binary search is valid. A baker writing them
    // out of order must not produce wrong lookups.
    MapBuilder b;
    b.AddTileset("cccc", 200);
    b.AddTileset("aaaa", 1);
    b.AddTileset("bbbb", 100);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    ASSERT_EQ(map->Tilesets().size(), 3u);
    EXPECT_EQ(map->Tilesets()[0].firstGid, 1u);
    EXPECT_EQ(map->Tilesets()[1].firstGid, 100u);
    EXPECT_EQ(map->Tilesets()[2].firstGid, 200u);

    uint32_t local = 0;
    size_t index = 999;
    const TilesetRef* t = map->ResolveTilesetWithIndex(250, local, index);
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->guid, "cccc");
    EXPECT_EQ(local, 50u);
    EXPECT_EQ(index, 2u) << "the index must address the sorted table";
}

TEST_F(MapFixture, AMapWithNoTilesetsResolvesNothing)
{
    MapBuilder b;
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    uint32_t local = 0;
    EXPECT_EQ(map->ResolveTileset(1, local), nullptr);
}

// ---- Bracketing the chunk index --------------------------------------------

TEST_F(MapFixture, AVisibleRectReturnsOnlyTheChunksInside)
{
    MapBuilder b;
    for (int32_t y = 0; y < 4; ++y)
        for (int32_t x = 0; x < 4; ++x)
            b.AddChunk(0, x, y);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    std::vector<ChunkIndexEntry> out;
    map->QueryVisibleChunks(0, 1, 1, 2, 2, out);

    EXPECT_EQ(out.size(), 4u) << "a 2x2 window over a 4x4 grid";
    for (const auto& e : out)
    {
        EXPECT_GE(e.chunkX, 1);
        EXPECT_LE(e.chunkX, 2);
        EXPECT_GE(e.chunkY, 1);
        EXPECT_LE(e.chunkY, 2);
    }
}

TEST_F(MapFixture, AQueryIsConfinedToItsLayer)
{
    MapBuilder b;
    b.AddChunk(0, 0, 0);
    b.AddChunk(1, 0, 0);
    b.AddChunk(1, 1, 0);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    std::vector<ChunkIndexEntry> out;
    map->QueryVisibleChunks(1, -10, -10, 10, 10, out);

    ASSERT_EQ(out.size(), 2u);
    for (const auto& e : out)
        EXPECT_EQ(e.layerIndex, 1) << "layer 0's chunk must not leak in";
}

TEST_F(MapFixture, ARectOutsideTheMapReturnsNothing)
{
    MapBuilder b;
    b.AddChunk(0, 0, 0);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    std::vector<ChunkIndexEntry> out;
    map->QueryVisibleChunks(0, 50, 50, 60, 60, out);
    EXPECT_TRUE(out.empty());
}

TEST_F(MapFixture, NegativeChunkCoordinatesWork)
{
    // An infinite Tiled map is authored around the origin, so chunks at
    // negative coordinates are ordinary, not an edge case.
    MapBuilder b;
    b.AddChunk(0, -2, -2);
    b.AddChunk(0, -1, -1);
    b.AddChunk(0, 0, 0);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    std::vector<ChunkIndexEntry> out;
    map->QueryVisibleChunks(0, -2, -2, -1, -1, out);
    EXPECT_EQ(out.size(), 2u);
}

TEST_F(MapFixture, TheQueryClearsWhatTheCallerPassedIn)
{
    MapBuilder b;
    b.AddChunk(0, 0, 0);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    std::vector<ChunkIndexEntry> out(7);  // caller's buffer, reused across frames
    map->QueryVisibleChunks(0, 99, 99, 99, 99, out);
    EXPECT_TRUE(out.empty()) << "stale entries from a previous frame must not survive";
}

TEST_F(MapFixture, TheHeaderIsReadBack)
{
    MapBuilder b;
    b.AddChunk(0, 0, 0);
    Tilemap* map = LoadFrom(b);
    ASSERT_NE(map, nullptr);

    EXPECT_EQ(map->MapWidth(), 64u);
    EXPECT_EQ(map->MapHeight(), 64u);
    EXPECT_EQ(map->TileWidth(), 16);
    EXPECT_EQ(map->TileHeight(), 16);
    EXPECT_EQ(map->ChunkWidth(), 8);
    EXPECT_EQ(map->ChunkHeight(), 8);
    EXPECT_EQ(map->LayerCount(), 2u);
    EXPECT_FALSE(map->IsInfinite()) << "flags bit 0 was not set";
}
