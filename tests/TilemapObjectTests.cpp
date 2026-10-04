// Map objects carry what Tiled gave them: properties, and the points of a
// polygon or polyline.
//
// The baker wrote every property, but the loader never read them back: the
// header did not say where they were, so Properties() was always empty and
// the spawner's scene_guid never matched. Polygon points were counted but
// never written at all. These tests bake a map with the editor's baker, load
// it the way a device does, and read the objects back.
//
// Also here: a tileset whose animation frame counts add up past 32 bits is
// refused instead of being read into a buffer sized from the wrapped total.

#include <gtest/gtest.h>

#include "Tilemap.h"
#include "Tileset.h"
#include "editor/TilemapBaker.h"

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace DekiTiledMap;

namespace
{
class MemoryFileSystem : public Deki::IFileSystem
{
public:
    MemoryFileSystem(std::string path, std::vector<uint8_t> bytes)
        : m_Path(std::move(path)),
          m_Bytes(std::move(bytes))
    {
    }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    FileHandle OpenFile(const char* path, OpenMode mode) override
    {
        if (mode != OpenMode::READ_BINARY || !path || m_Path != path)
        {
            return nullptr;
        }
        m_Cursor = 0;
        return reinterpret_cast<FileHandle>(this);
    }
    void CloseFile(FileHandle) override {}
    size_t ReadFile(FileHandle, void* buffer, size_t size) override
    {
        if (m_Cursor >= m_Bytes.size())
        {
            return 0;
        }
        const size_t n = std::min(size, m_Bytes.size() - m_Cursor);
        std::memcpy(buffer, m_Bytes.data() + m_Cursor, n);
        m_Cursor += n;
        return n;
    }
    size_t WriteFile(FileHandle, const void*, size_t) override { return 0; }
    long SeekFile(FileHandle, long offset, SeekOrigin origin) override
    {
        long base = 0;
        if (origin == SeekOrigin::CURRENT)
        {
            base = static_cast<long>(m_Cursor);
        }
        else if (origin == SeekOrigin::END)
        {
            base = static_cast<long>(m_Bytes.size());
        }
        long target = base + offset;
        if (target < 0)
        {
            target = 0;
        }
        if (target > static_cast<long>(m_Bytes.size()))
        {
            target = static_cast<long>(m_Bytes.size());
        }
        m_Cursor = static_cast<size_t>(target);
        return target;
    }
    long TellFile(FileHandle) override { return static_cast<long>(m_Cursor); }
    long GetFileSize(FileHandle) override { return static_cast<long>(m_Bytes.size()); }
    bool FileExists(const char* path) override { return path && m_Path == path; }
    bool ConvertPath(const char* virtualPath, char* out, size_t cap) override
    {
        if (!virtualPath || !out)
        {
            return false;
        }
        std::snprintf(out, cap, "%s", virtualPath);
        return true;
    }

private:
    std::string m_Path;
    std::vector<uint8_t> m_Bytes;
    size_t m_Cursor = 0;
};

TmjPropertyValue Prop(const char* name, const char* type)
{
    TmjPropertyValue p;
    p.name = name;
    p.type = type;
    return p;
}

// Two layers, so property and point indices have to carry across them.
TmjMap MapWithObjects(bool withPolygons)
{
    TmjMap map;
    map.width = 4;
    map.height = 4;
    map.tileWidth = 16;
    map.tileHeight = 16;
    map.chunkWidth = 4;
    map.chunkHeight = 4;

    TmjObjectLayer spawns;
    spawns.name = "spawns";
    TmjObject door;
    door.id = 1;
    door.name = "door";
    door.x = 32;
    door.y = 48;
    TmjPropertyValue guid = Prop("scene_guid", "string");
    guid.svalue = "0123456789abcdef0123456789abcdef";
    TmjPropertyValue hp = Prop("hp", "int");
    hp.ivalue = 7;
    door.properties = { guid, hp };
    spawns.objects.push_back(door);

    TmjObject plain;
    plain.id = 2;
    plain.name = "plain";
    spawns.objects.push_back(plain);
    map.objectLayers.push_back(spawns);

    TmjObjectLayer shapes;
    shapes.name = "shapes";
    TmjObject zone;
    zone.id = 3;
    zone.name = "zone";
    if (withPolygons)
    {
        zone.polygonPoints = { 0, 0, 10, 0, 10, 20 };
    }
    TmjPropertyValue speed = Prop("speed", "float");
    speed.fvalue = 1.5f;
    TmjPropertyValue on = Prop("on", "bool");
    on.bvalue = true;
    zone.properties = { speed, on };
    shapes.objects.push_back(zone);
    if (withPolygons)
    {
        TmjObject path;
        path.id = 4;
        path.name = "path";
        path.polylinePoints = { 1, 2, 3, 4 };
        shapes.objects.push_back(path);
    }
    map.objectLayers.push_back(shapes);
    return map;
}

std::vector<uint8_t> Bake(const TmjMap& map)
{
    const fs::path out = fs::temp_directory_path() / "deki_tilemap_object_test.dtilemap";
    EXPECT_TRUE(WriteDtilemap(map, {}, out.string()));
    std::ifstream in(out, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    fs::remove(out, ec);
    return bytes;
}

DTilemapHeader& HeaderOf(std::vector<uint8_t>& bytes)
{
    return *reinterpret_cast<DTilemapHeader*>(bytes.data());
}

const DTilemapObject* Find(const Tilemap& map, const char* name)
{
    for (const auto& o : map.Objects())
    {
        if (std::strncmp(o.name, name, sizeof(o.name)) == 0)
        {
            return &o;
        }
    }
    return nullptr;
}

const DTilemapProperty* FindProp(const Tilemap& map, const DTilemapObject& obj, const char* name)
{
    uint32_t count = 0;
    const DTilemapProperty* props = map.ObjectProperties(obj, count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (map.GetString(props[i].nameOffset) == name)
        {
            return &props[i];
        }
    }
    return nullptr;
}

class ObjectsFromS : public ::testing::Test
{
protected:
    Tilemap* Load(std::vector<uint8_t> bytes)
    {
        m_Fs = std::make_unique<MemoryFileSystem>("S:/maps/level.dtilemap", std::move(bytes));
        Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
        Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
        return Tilemap::Load("S:/maps/level.dtilemap");
    }
    Tileset* LoadTileset(std::vector<uint8_t> bytes)
    {
        m_Fs = std::make_unique<MemoryFileSystem>("S:/maps/tiles.dtileset", std::move(bytes));
        Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
        Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
        return Tileset::Load("S:/maps/tiles.dtileset");
    }
    void TearDown() override
    {
        Deki::FileSystem::UnregisterFileSystem("S:/");
        Deki::FileSystem::SetDefaultFileSystem(nullptr);
        m_Fs.reset();
    }

private:
    std::unique_ptr<MemoryFileSystem> m_Fs;
};

void ExpectEveryProperty(const Tilemap& map)
{
    const DTilemapObject* door = Find(map, "door");
    ASSERT_NE(door, nullptr);
    const DTilemapProperty* guid = FindProp(map, *door, "scene_guid");
    ASSERT_NE(guid, nullptr) << "the spawner looks for exactly this property";
    EXPECT_EQ(guid->type, static_cast<uint32_t>(DPropertyType::String));
    EXPECT_EQ(map.GetString(guid->valueOffset), "0123456789abcdef0123456789abcdef");
    const DTilemapProperty* hp = FindProp(map, *door, "hp");
    ASSERT_NE(hp, nullptr);
    EXPECT_EQ(hp->intValue, 7);

    const DTilemapObject* plain = Find(map, "plain");
    ASSERT_NE(plain, nullptr);
    uint32_t count = 99;
    EXPECT_EQ(map.ObjectProperties(*plain, count), nullptr);
    EXPECT_EQ(count, 0u);

    // On the second layer: its indices continue from the first.
    const DTilemapObject* zone = Find(map, "zone");
    ASSERT_NE(zone, nullptr);
    const DTilemapProperty* speed = FindProp(map, *zone, "speed");
    ASSERT_NE(speed, nullptr);
    EXPECT_FLOAT_EQ(speed->floatValue, 1.5f);
    const DTilemapProperty* on = FindProp(map, *zone, "on");
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->boolValue, 1u);
}
}  // namespace

TEST_F(ObjectsFromS, PropertiesLoadForEveryObject)
{
    Tilemap* map = Load(Bake(MapWithObjects(true)));
    ASSERT_NE(map, nullptr);
    ExpectEveryProperty(*map);
    delete map;
}

TEST_F(ObjectsFromS, PolygonAndPolylinePointsLoad)
{
    Tilemap* map = Load(Bake(MapWithObjects(true)));
    ASSERT_NE(map, nullptr);
    const auto& pts = map->PolygonPoints();

    const DTilemapObject* zone = Find(*map, "zone");
    ASSERT_NE(zone, nullptr);
    EXPECT_EQ(zone->shape, static_cast<uint32_t>(DObjectShape::Polygon));
    ASSERT_EQ(zone->pointCount, 3u);
    ASSERT_LE(size_t(zone->pointOffset + zone->pointCount) * 2, pts.size());
    const int32_t* z = pts.data() + zone->pointOffset * 2;
    EXPECT_EQ(std::vector<int32_t>(z, z + 6), (std::vector<int32_t>{ 0, 0, 10, 0, 10, 20 }));

    const DTilemapObject* path = Find(*map, "path");
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->shape, static_cast<uint32_t>(DObjectShape::Polyline));
    ASSERT_EQ(path->pointCount, 2u);
    ASSERT_LE(size_t(path->pointOffset + path->pointCount) * 2, pts.size());
    const int32_t* p = pts.data() + path->pointOffset * 2;
    EXPECT_EQ(std::vector<int32_t>(p, p + 4), (std::vector<int32_t>{ 1, 2, 3, 4 }));
    delete map;
}

// A map baked before the header named its pools: same layout, no flag, the
// pool fields zero (they were padding). Its properties are found from where
// that baker put them.
TEST_F(ObjectsFromS, AMapBakedBeforeThePoolOffsetsKeepsItsProperties)
{
    std::vector<uint8_t> bytes = Bake(MapWithObjects(false));
    DTilemapHeader& h = HeaderOf(bytes);
    ASSERT_EQ(h.pointPoolCount, 0u);
    h.flags &= ~kTilemapHasPools;
    h.pointPoolOffset = h.pointPoolCount = h.propertyTableOffset = h.propertyCount = h.stringPoolOffset = 0;

    Tilemap* map = Load(std::move(bytes));
    ASSERT_NE(map, nullptr);
    ExpectEveryProperty(*map);
    delete map;
}

TEST_F(ObjectsFromS, DamagedPropertiesLeaveTheMapLoading)
{
    std::vector<uint8_t> bytes = Bake(MapWithObjects(true));
    HeaderOf(bytes).propertyCount = 0x7FFFFFFF;

    Tilemap* map = Load(std::move(bytes));
    ASSERT_NE(map, nullptr) << "the tiles do not need the properties";
    EXPECT_TRUE(map->Properties().empty());
    const DTilemapObject* door = Find(*map, "door");
    ASSERT_NE(door, nullptr);
    EXPECT_EQ(FindProp(*map, *door, "scene_guid"), nullptr);
    delete map;
}

TEST_F(ObjectsFromS, ATilesetWhoseFrameCountsWrapIsRefused)
{
    DTilesetHeader h{};
    std::memcpy(h.magic, "DTS1", 4);
    h.version = 1;
    h.animCount = 2;
    h.animTableOffset = sizeof(DTilesetHeader);

    DTileAnimation a[2]{};
    a[0].frameOffset = sizeof(DTilesetHeader) + sizeof(a);
    a[0].frameCount = 0x80000000u;
    a[1].frameOffset = a[0].frameOffset;
    a[1].frameCount = 0x80000001u;  // the uint32 sum is 1

    DTileAnimationFrame frame{};
    std::vector<uint8_t> bytes(sizeof(h) + sizeof(a) + sizeof(frame));
    std::memcpy(bytes.data(), &h, sizeof(h));
    std::memcpy(bytes.data() + sizeof(h), a, sizeof(a));
    std::memcpy(bytes.data() + sizeof(h) + sizeof(a), &frame, sizeof(frame));

    EXPECT_EQ(LoadTileset(std::move(bytes)), nullptr);
}

TEST_F(ObjectsFromS, ATilesetWithAHugeTableIsRefused)
{
    DTilesetHeader h{};
    std::memcpy(h.magic, "DTS1", 4);
    h.version = 1;
    h.collisionCount = 0x40000000u;
    h.collisionTableOffset = sizeof(DTilesetHeader);
    std::vector<uint8_t> bytes(sizeof(h));
    std::memcpy(bytes.data(), &h, sizeof(h));

    EXPECT_EQ(LoadTileset(std::move(bytes)), nullptr);
}
