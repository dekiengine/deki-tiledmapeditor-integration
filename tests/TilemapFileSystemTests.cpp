// The loaders must read through the engine filesystem, not stdio.
//
// The asset manager hands a loader the cache directory joined to the asset's
// path, and that directory is the "S:/" mount on a device and in the desktop
// simulator. Only IFileSystem knows how to resolve that prefix: to the SD card
// on a board, to ./storage/ beside the executable in a simulator. Raw
// std::fopen sees "S:/..." as a drive that does not exist and fails.
//
// Both loaders used std::fopen, so a tilemap loaded in the editor — where the
// cache directory is a real native path — and nowhere else. The chunk streamer
// set up at the end of Tilemap::Load had always read through the filesystem, so
// one function used both routes.
//
// These tests serve the file from an in-memory filesystem mounted at "S:/",
// which is exactly the case stdio cannot satisfy.

#include <gtest/gtest.h>

#include "Tilemap.h"
#include "Tileset.h"

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{
// Serves one file, by exact path, out of a byte vector. Records whether it was
// asked for anything, so a test can tell "read through the filesystem" from
// "happened to work".
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
        if (mode != OpenMode::READ_BINARY || !path || m_Path != path)
            return nullptr;
        ++opens;
        m_Cursor = 0;
        return reinterpret_cast<FileHandle>(this);
    }
    void CloseFile(FileHandle) override { ++closes; }

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
        long target = base + offset;
        if (target < 0) target = 0;
        if (target > static_cast<long>(m_Bytes.size())) target = static_cast<long>(m_Bytes.size());
        m_Cursor = static_cast<size_t>(target);
        return target;
    }
    long TellFile(FileHandle) override { return static_cast<long>(m_Cursor); }
    long GetFileSize(FileHandle) override { return static_cast<long>(m_Bytes.size()); }
    bool FileExists(const char* path) override { return path && m_Path == path; }

    bool ConvertPath(const char* virtualPath, char* out, size_t cap) override
    {
        if (!virtualPath || !out) return false;
        std::snprintf(out, cap, "%s", virtualPath);
        return true;
    }

    int opens = 0;
    int closes = 0;

private:
    std::string m_Path;
    std::vector<uint8_t> m_Bytes;
    size_t m_Cursor = 0;
};

std::vector<uint8_t> MinimalTilemapBytes()
{
    DekiTiledMap::DTilemapHeader h{};
    std::memcpy(h.magic, "DTM1", 4);
    h.version = 1;
    h.mapWidth = 4;
    h.mapHeight = 4;
    h.tileWidth = 16;
    h.tileHeight = 16;
    h.chunkWidth = 4;
    h.chunkHeight = 4;
    h.layerCount = 1;
    // No chunk index, tileset table or object layers: the header alone is a
    // valid file, and it is all this test needs to prove the read happened.
    std::vector<uint8_t> bytes(sizeof(h));
    std::memcpy(bytes.data(), &h, sizeof(h));
    return bytes;
}

std::vector<uint8_t> MinimalTilesetBytes()
{
    DekiTiledMap::DTilesetHeader h{};
    std::memcpy(h.magic, "DTS1", 4);
    h.version = 1;
    std::vector<uint8_t> bytes(sizeof(h));
    std::memcpy(bytes.data(), &h, sizeof(h));
    return bytes;
}

// Mounts a filesystem at "S:/" for the life of the test, as the SD card
// component does on a device.
class MountedAtS : public ::testing::Test
{
protected:
    void Mount(const std::string& path, std::vector<uint8_t> bytes)
    {
        m_Fs = std::make_unique<MemoryFileSystem>(path, std::move(bytes));
        Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
        Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
    }
    void TearDown() override
    {
        Deki::FileSystem::UnregisterFileSystem("S:/");
        Deki::FileSystem::SetDefaultFileSystem(nullptr);
        m_Fs.reset();
    }
    MemoryFileSystem& Fs() { return *m_Fs; }

private:
    std::unique_ptr<MemoryFileSystem> m_Fs;
};
}  // namespace

TEST_F(MountedAtS, ATilemapLoadsFromAMountedPath)
{
    Mount("S:/maps/level.dtilemap", MinimalTilemapBytes());

    DekiTiledMap::Tilemap* map = DekiTiledMap::Tilemap::Load("S:/maps/level.dtilemap");

    ASSERT_NE(map, nullptr) << "a path behind the S:/ mount must load; std::fopen could not";
    EXPECT_GT(Fs().opens, 0) << "the read went somewhere other than the filesystem";
    EXPECT_EQ(map->TileWidth(), 16);
    EXPECT_EQ(map->MapWidth(), 4u);
    EXPECT_EQ(map->LayerCount(), 1u);
    delete map;
}

TEST_F(MountedAtS, ATilesetLoadsFromAMountedPath)
{
    Mount("S:/maps/tiles.dtileset", MinimalTilesetBytes());

    DekiTiledMap::Tileset* ts = DekiTiledMap::Tileset::Load("S:/maps/tiles.dtileset");

    ASSERT_NE(ts, nullptr);
    EXPECT_GT(Fs().opens, 0);
    delete ts;
}

TEST_F(MountedAtS, EveryHandleIsClosedByTheTimeTheMapIsGone)
{
    Mount("S:/maps/level.dtilemap", MinimalTilemapBytes());
    DekiTiledMap::Tilemap* map = DekiTiledMap::Tilemap::Load("S:/maps/level.dtilemap");
    ASSERT_NE(map, nullptr);

    // Two opens, deliberately: Load reads the header and closes, then hands the
    // path to the streamer, which keeps its own handle open for chunk reads
    // until the map is destroyed.
    EXPECT_EQ(Fs().opens, 2);
    EXPECT_EQ(Fs().closes, 1) << "the header handle is closed before Load returns";

    delete map;
    EXPECT_EQ(Fs().closes, Fs().opens) << "the streamer must release its handle too";
}

TEST_F(MountedAtS, AMissingFileIsRefusedNotCrashed)
{
    Mount("S:/maps/level.dtilemap", MinimalTilemapBytes());
    EXPECT_EQ(DekiTiledMap::Tilemap::Load("S:/maps/nope.dtilemap"), nullptr);
    EXPECT_EQ(DekiTiledMap::Tileset::Load("S:/maps/nope.dtileset"), nullptr);
}

TEST_F(MountedAtS, AFileWithTheWrongMagicIsRefused)
{
    std::vector<uint8_t> junk = MinimalTilemapBytes();
    junk[0] = 'X';
    Mount("S:/maps/level.dtilemap", std::move(junk));

    EXPECT_EQ(DekiTiledMap::Tilemap::Load("S:/maps/level.dtilemap"), nullptr);
    EXPECT_EQ(Fs().closes, Fs().opens) << "the handle must be closed on the reject path too";
}

TEST_F(MountedAtS, ATruncatedHeaderIsRefused)
{
    std::vector<uint8_t> partial = MinimalTilemapBytes();
    partial.resize(partial.size() / 2);
    Mount("S:/maps/level.dtilemap", std::move(partial));

    EXPECT_EQ(DekiTiledMap::Tilemap::Load("S:/maps/level.dtilemap"), nullptr);
}

TEST(TilemapLoaderArguments, ANullPathIsRefused)
{
    EXPECT_EQ(DekiTiledMap::Tilemap::Load(nullptr), nullptr);
    EXPECT_EQ(DekiTiledMap::Tileset::Load(nullptr), nullptr);
}
