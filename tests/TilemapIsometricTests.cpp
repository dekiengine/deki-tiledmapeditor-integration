// Isometric maps: the orientation survives the bake, the grid maths matches
// what Tiled draws, and a layout the renderer does not know is refused rather
// than drawn as something else.

#include <gtest/gtest.h>

#include "Tilemap.h"
#include "editor/TilemapBaker.h"
#include "editor/TmjParser.h"

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>

#include <algorithm>
#include <cstdio>
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
class OneFile : public Deki::IFileSystem
{
public:
    OneFile(std::string path, std::vector<uint8_t> bytes)
        : m_Path(std::move(path)),
          m_Bytes(std::move(bytes))
    {
    }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    FileHandle OpenFile(const char* path, OpenMode mode) override
    {
        if (mode != OpenMode::ReadBinary || !path || m_Path != path)
        {
            return nullptr;
        }
        m_Cursor = 0;
        return reinterpret_cast<FileHandle>(this);
    }
    void CloseFile(FileHandle) override {}
    size_t ReadFile(FileHandle, void* buffer, size_t size) override
    {
        const size_t n = m_Cursor < m_Bytes.size() ? std::min(size, m_Bytes.size() - m_Cursor) : 0;
        if (n > 0)
        {
            std::memcpy(buffer, m_Bytes.data() + m_Cursor, n);
            m_Cursor += n;
        }
        return n;
    }
    size_t WriteFile(FileHandle, const void*, size_t) override { return 0; }
    long SeekFile(FileHandle, long offset, SeekOrigin origin) override
    {
        const long base = origin == SeekOrigin::Current ? static_cast<long>(m_Cursor)
                          : origin == SeekOrigin::End   ? static_cast<long>(m_Bytes.size())
                                                        : 0;
        m_Cursor = static_cast<size_t>(std::clamp(base + offset, 0L, static_cast<long>(m_Bytes.size())));
        return static_cast<long>(m_Cursor);
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

TmjMap SmallMap(bool isometric)
{
    TmjMap map;
    map.width = 4;
    map.height = 4;
    map.tileWidth = 32;
    map.tileHeight = 16;
    map.chunkWidth = 4;
    map.chunkHeight = 4;
    map.isometric = isometric;
    return map;
}

class IsometricMap : public ::testing::Test
{
protected:
    Tilemap* Load(const TmjMap& source)
    {
        const fs::path out = fs::temp_directory_path() / "deki_tilemap_iso_test.dtilemap";
        EXPECT_TRUE(WriteDtilemap(source, {}, out.string()));
        std::ifstream in(out, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        fs::remove(out, ec);

        m_Fs = std::make_unique<OneFile>("S:/maps/level.dtilemap", std::move(bytes));
        Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
        Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
        m_Map.reset(Tilemap::Load("S:/maps/level.dtilemap"));
        return m_Map.get();
    }

    std::unique_ptr<OneFile> m_Fs;
    std::unique_ptr<Tilemap> m_Map;
};

std::string WriteTmj(const std::string& orientationField)
{
    const fs::path path = fs::temp_directory_path() / "deki_tilemap_iso_test.tmj";
    std::ofstream out(path);
    out << "{" << orientationField
        << R"("width":2,"height":2,"tilewidth":32,"tileheight":16,"infinite":false,"tilesets":[],"layers":[]})";
    return path.string();
}
}  // namespace

TEST_F(IsometricMap, TheOrientationSurvivesTheBake)
{
    Tilemap* iso = Load(SmallMap(true));
    ASSERT_NE(iso, nullptr);
    EXPECT_TRUE(iso->IsIsometric());
}

TEST_F(IsometricMap, AMapBakedAsOrthogonalStaysOrthogonal)
{
    Tilemap* plain = Load(SmallMap(false));
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->IsIsometric());
}

TEST_F(IsometricMap, TilesLandOnTheDiamondGrid)
{
    Tilemap* map = Load(SmallMap(true));
    ASSERT_NE(map, nullptr);
    float x = 0.0f, y = 0.0f;

    map->TileToPixels(1.0f, 0.0f, x, y);  // a column goes down and right
    EXPECT_FLOAT_EQ(x, 16.0f);
    EXPECT_FLOAT_EQ(y, 8.0f);

    map->TileToPixels(0.0f, 1.0f, x, y);  // a row goes down and left
    EXPECT_FLOAT_EQ(x, -16.0f);
    EXPECT_FLOAT_EQ(y, 8.0f);
}

TEST_F(IsometricMap, PixelsToTileUndoesTileToPixels)
{
    Tilemap* map = Load(SmallMap(true));
    ASSERT_NE(map, nullptr);
    for (float column : { 0.0f, 1.5f, 7.0f, -3.0f })
    {
        for (float row : { 0.0f, 2.25f, -4.0f })
        {
            float x = 0.0f, y = 0.0f, backColumn = 0.0f, backRow = 0.0f;
            map->TileToPixels(column, row, x, y);
            map->PixelsToTile(x, y, backColumn, backRow);
            EXPECT_NEAR(backColumn, column, 1e-4f);
            EXPECT_NEAR(backRow, row, 1e-4f);
        }
    }
}

TEST_F(IsometricMap, OrthogonalMathsIsUnchanged)
{
    Tilemap* map = Load(SmallMap(false));
    ASSERT_NE(map, nullptr);
    float x = 0.0f, y = 0.0f;
    map->TileToPixels(2.0f, 3.0f, x, y);
    EXPECT_FLOAT_EQ(x, 64.0f);
    EXPECT_FLOAT_EQ(y, 48.0f);
    map->ObjectToPixels(5.0f, 9.0f, x, y);
    EXPECT_FLOAT_EQ(x, 5.0f);
    EXPECT_FLOAT_EQ(y, 9.0f);
    map->GetAnchor(x, y);
    EXPECT_FLOAT_EQ(x, 64.0f);
    EXPECT_FLOAT_EQ(y, 32.0f);
}

TEST_F(IsometricMap, AFiniteMapIsCentredOnItsDiamond)
{
    Tilemap* map = Load(SmallMap(true));
    ASSERT_NE(map, nullptr);
    float x = 1.0f, y = 1.0f;
    map->GetAnchor(x, y);
    EXPECT_FLOAT_EQ(x, 0.0f);   // 4 x 4: the top and bottom points line up
    EXPECT_FLOAT_EQ(y, 32.0f);  // half of (4 + 4) * 8
}

TEST_F(IsometricMap, ObjectsAreMeasuredInTileHeightsAlongTheAxes)
{
    Tilemap* map = Load(SmallMap(true));
    ASSERT_NE(map, nullptr);
    float x = 0.0f, y = 0.0f;
    map->ObjectToPixels(32.0f, 0.0f, x, y);  // Two tiles along the column axis.
    EXPECT_FLOAT_EQ(x, 32.0f);
    EXPECT_FLOAT_EQ(y, 16.0f);
}

TEST_F(IsometricMap, AnOriginObjectIsProjectedToo)
{
    TmjMap source = SmallMap(true);
    source.infinite = true;
    TmjObjectLayer layer;
    layer.name = "marks";
    TmjObject origin;
    origin.id = 1;
    origin.name = "origin";
    origin.x = 16;
    origin.y = 16;  // one tile along each axis
    origin.width = origin.height = 0;
    origin.rotation = 0.0f;
    layer.objects.push_back(origin);
    source.objectLayers.push_back(layer);

    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);
    float x = 1.0f, y = 1.0f;
    map->GetAnchor(x, y);
    EXPECT_FLOAT_EQ(x, 0.0f);
    EXPECT_FLOAT_EQ(y, 16.0f);
}

TEST(TmjOrientation, IsometricIsRead)
{
    TmjMap map;
    std::string error;
    ASSERT_TRUE(ParseTmjMap(WriteTmj(R"("orientation":"isometric",)"), map, error)) << error;
    EXPECT_TRUE(map.isometric);
}

TEST(TmjOrientation, OrthogonalIsTheDefault)
{
    TmjMap map;
    std::string error;
    ASSERT_TRUE(ParseTmjMap(WriteTmj(""), map, error)) << error;
    EXPECT_FALSE(map.isometric);
}

TEST(TmjOrientation, StaggeredAndHexagonalAreRead)
{
    TmjMap map;
    std::string error;
    ASSERT_TRUE(ParseTmjMap(
        WriteTmj(R"("orientation":"hexagonal","staggeraxis":"x","staggerindex":"even","hexsidelength":6,)"), map,
        error))
        << error;
    EXPECT_TRUE(map.hexagonal);
    EXPECT_FALSE(map.staggered);
    EXPECT_TRUE(map.staggerX);
    EXPECT_TRUE(map.staggerEven);
    EXPECT_EQ(map.hexSideLength, 6);

    TmjMap staggered;
    ASSERT_TRUE(ParseTmjMap(WriteTmj(R"("orientation":"staggered",)"), staggered, error)) << error;
    EXPECT_TRUE(staggered.staggered);
    EXPECT_FALSE(staggered.staggerX) << "Tiled's default axis is Y";
    EXPECT_FALSE(staggered.staggerEven) << "and its default index odd";
}

TEST(TmjOrientation, OtherLayoutsAreRefusedByName)
{
    for (const char* orientation : { "triangular" })
    {
        TmjMap map;
        std::string error;
        EXPECT_FALSE(ParseTmjMap(WriteTmj(std::string("\"orientation\":\"") + orientation + "\","), map, error));
        EXPECT_NE(error.find(orientation), std::string::npos) << error;
    }
}

namespace
{
DTilemapObject TileObject(int32_t x, int32_t y, float rotation = 0.0f)
{
    DTilemapObject obj{};
    obj.x = x;
    obj.y = y;
    obj.rotation = rotation;
    return obj;
}
}  // namespace

TEST_F(IsometricMap, ATileObjectStandsBottomCentreOnItsPoint)
{
    TmjMap source = SmallMap(true);
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);

    // Two tiles along each axis is straight down the middle of the diamond grid.
    float x = 0.0f, y = 0.0f;
    map->TileObjectCentre(TileObject(32, 32), 32.0f, 48.0f, TileObjectAlignment::Unspecified, x, y);
    EXPECT_FLOAT_EQ(x, 0.0f);
    EXPECT_FLOAT_EQ(y, 32.0f - 24.0f);
}

TEST_F(IsometricMap, ATileObjectOnASquareMapStandsBottomLeftOnItsPoint)
{
    TmjMap source = SmallMap(false);
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);

    float x = 0.0f, y = 0.0f;
    map->TileObjectCentre(TileObject(100, 50), 32.0f, 48.0f, TileObjectAlignment::Unspecified, x, y);
    EXPECT_FLOAT_EQ(x, 116.0f);
    EXPECT_FLOAT_EQ(y, 26.0f);
}

TEST_F(IsometricMap, ATileObjectTurnsAroundItsPoint)
{
    TmjMap source = SmallMap(false);
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);

    // A quarter turn clockwise takes the image from above the point to its right.
    float x = 0.0f, y = 0.0f;
    map->TileObjectCentre(TileObject(0, 0, 90.0f), 32.0f, 48.0f, TileObjectAlignment::Unspecified, x, y);
    EXPECT_NEAR(x, 24.0f, 0.001f);
    EXPECT_NEAR(y, 16.0f, 0.001f);
}

TEST_F(IsometricMap, ATilesetsMarginAndSpacingMoveItsTiles)
{
    const fs::path tsj = fs::temp_directory_path() / "deki_tilemap_iso_spaced.tsj";
    {
        std::ofstream out(tsj);
        out << R"({"tilewidth":96,"tileheight":96,"margin":2,"spacing":64,"columns":2,"tilecount":4,"image":"a.png"})";
    }
    TmjTileset parsed;
    std::string error;
    ASSERT_TRUE(ParseTsjTileset(tsj.string(), parsed, error)) << error;
    EXPECT_EQ(parsed.margin, 2);
    EXPECT_EQ(parsed.spacing, 64);

    const fs::path baked = fs::temp_directory_path() / "deki_tilemap_iso_spaced.dtileset";
    ASSERT_TRUE(WriteDtileset(parsed, "0123456789abcdef0123456789abcdef", baked.string()));
    std::ifstream in(baked, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    fs::remove(baked, ec);
    fs::remove(tsj, ec);

    m_Fs = std::make_unique<OneFile>("S:/maps/spaced.dtileset", std::move(bytes));
    Deki::FileSystem::RegisterFileSystem("S:/", m_Fs.get());
    Deki::FileSystem::SetDefaultFileSystem(m_Fs.get());
    std::unique_ptr<Tileset> tileset(Tileset::Load("S:/maps/spaced.dtileset"));
    ASSERT_NE(tileset, nullptr);

    int x = 0, y = 0, w = 0, h = 0;
    tileset->GetTileRect(3, x, y, w, h);
    EXPECT_EQ(x, 2 + 96 + 64);
    EXPECT_EQ(y, 2 + 96 + 64);
    EXPECT_EQ(w, 96);
    EXPECT_EQ(h, 96);
}

TEST_F(IsometricMap, ATilesetsAlignmentPicksThePointOfTheImage)
{
    TmjMap source = SmallMap(false);
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);

    float x = 0.0f, y = 0.0f;
    map->TileObjectCentre(TileObject(100, 50), 32.0f, 48.0f, TileObjectAlignment::Center, x, y);
    EXPECT_FLOAT_EQ(x, 100.0f);
    EXPECT_FLOAT_EQ(y, 50.0f);
    map->TileObjectCentre(TileObject(100, 50), 32.0f, 48.0f, TileObjectAlignment::TopRight, x, y);
    EXPECT_FLOAT_EQ(x, 84.0f);
    EXPECT_FLOAT_EQ(y, 74.0f);
}

TEST_F(IsometricMap, AnObjectOnAHalfPixelGoesToThePixelAbove)
{
    TmjMap source = SmallMap(true);
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);

    // 3 and 2 tiles along the axes: (3 + 2) * 8 is on a whole pixel, but one
    // unit further along is on a half.
    float x = 0.0f, y = 0.0f;
    map->TileObjectCentre(TileObject(49, 32), 32.0f, 32.0f, TileObjectAlignment::Unspecified, x, y);
    EXPECT_FLOAT_EQ(y, 40.0f - 16.0f);
}

namespace
{
TmjMap ParseLayers(const std::string& layers)
{
    const fs::path path = fs::temp_directory_path() / "deki_tilemap_iso_layers.tmj";
    {
        std::ofstream out(path);
        out << R"({"width":1,"height":1,"tilewidth":16,"tileheight":16,"infinite":false,"tilesets":[],"layers":[)" << layers << "]}";
    }
    TmjMap map;
    std::string error;
    EXPECT_TRUE(ParseTmjMap(path.string(), map, error)) << error;
    std::error_code ec;
    fs::remove(path, ec);
    return map;
}
}  // namespace

TEST(TmjLayers, AGroupsLayersAreReadInOrderAndItsVisibilityCounts)
{
    const TmjMap map = ParseLayers(R"(
        {"type":"tilelayer","name":"a","width":1,"height":1,"data":[1]},
        {"type":"group","name":"g","visible":false,"layers":[{"type":"tilelayer","name":"b","width":1,"height":1,"data":[1]}]},
        {"type":"tilelayer","name":"c","visible":false,"width":1,"height":1,"data":[1]})");
    ASSERT_EQ(map.tileLayers.size(), 3u);
    EXPECT_EQ(map.tileLayers[1].name, "b");
    EXPECT_TRUE(map.tileLayers[0].visible);
    EXPECT_FALSE(map.tileLayers[1].visible);
    EXPECT_FALSE(map.tileLayers[2].visible);
}

TEST(TmjLayers, AHiddenTileObjectIsDroppedAndOtherHiddenObjectsStay)
{
    const TmjMap map = ParseLayers(R"(
        {"type":"objectgroup","name":"o","objects":[
            {"id":1,"gid":1,"visible":false},
            {"id":2,"gid":1},
            {"id":3,"visible":false},
            {"id":4,"gid":1,"visible":false,"properties":[{"name":"scene_guid","type":"string","value":"x"}]}]})");
    ASSERT_EQ(map.objectLayers.size(), 1u);
    ASSERT_EQ(map.objectLayers[0].objects.size(), 3u);
    EXPECT_EQ(map.objectLayers[0].objects[0].id, 2u);
    EXPECT_EQ(map.objectLayers[0].objects[1].id, 3u);
    EXPECT_EQ(map.objectLayers[0].objects[2].id, 4u);
}

TEST_F(IsometricMap, AHiddenLayerIsInTheFileAndMaskedOffTheDrawing)
{
    TmjMap source = SmallMap(true);
    TmjLayer shown;
    shown.name = "shown";
    shown.width = shown.height = 4;
    shown.data.assign(16, 1);
    TmjLayer hidden = shown;
    hidden.name = "hidden";
    hidden.visible = false;
    source.tileLayers = { shown, hidden };
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->LayerCount(), 2u);
    EXPECT_EQ(map->HiddenLayerMask(), 2u);
}

// Staggered and hexagonal maps lay tiles out as Tiled's HexagonalRenderer.
TEST_F(IsometricMap, AStaggeredMapShiftsTheOddRowsByHalfATile)
{
    TmjMap source = SmallMap(false);
    source.staggered = true;
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(map->IsStaggered());
    EXPECT_FALSE(map->IsHexagonal());
    const Tilemap::StaggerGrid& g = map->Stagger();
    int32_t x = -1, y = -1;
    g.TileCorner(0, 1, x, y);
    EXPECT_EQ(x, 16);
    EXPECT_EQ(y, 8);
    g.TileCorner(2, 2, x, y);
    EXPECT_EQ(x, 64);
    EXPECT_EQ(y, 16);

    int32_t w = 0, h = 0;
    ASSERT_TRUE(map->MapPixelSize(w, h));
    EXPECT_EQ(w, 4 * 32 + 16);
    EXPECT_EQ(h, 4 * 8 + 8);
    float ax = 0.0f, ay = 0.0f;
    map->GetAnchor(ax, ay);
    EXPECT_FLOAT_EQ(ax, 72.0f) << "the middle of the map's pixels";
    EXPECT_FLOAT_EQ(ay, 20.0f);

    // A tile's middle picks that tile, shifted rows too.
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            g.TileCorner(column, row, x, y);
            float c = -1.0f, r = -1.0f;
            map->PixelsToTile(static_cast<float>(x + 16), static_cast<float>(y + 8), c, r);
            EXPECT_EQ(c, static_cast<float>(column)) << column << "," << row;
            EXPECT_EQ(r, static_cast<float>(row)) << column << "," << row;
        }
    }

    float ox = 0.0f, oy = 0.0f;
    map->ObjectToPixels(40.0f, 12.5f, ox, oy);
    EXPECT_FLOAT_EQ(ox, 40.0f) << "objects are in plain pixels, as on a square map";
    EXPECT_FLOAT_EQ(oy, 12.5f);
    EXPECT_FLOAT_EQ(map->BoundsLeft(), 0.0f);
}

TEST_F(IsometricMap, AHexagonalMapsSidesWidenItsSteps)
{
    TmjMap source = SmallMap(false);
    source.tileHeight = 28;
    source.hexagonal = true;
    source.hexSideLength = 16;
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(map->IsHexagonal());
    const Tilemap::StaggerGrid& g = map->Stagger();
    EXPECT_EQ(g.columnWidth, 16);
    EXPECT_EQ(g.rowHeight, 22);
    int32_t x = 0, y = 0;
    g.TileCorner(0, 1, x, y);
    EXPECT_EQ(x, 16);
    EXPECT_EQ(y, 22);
    int32_t w = 0, h = 0;
    ASSERT_TRUE(map->MapPixelSize(w, h));
    EXPECT_EQ(w, 144);
    EXPECT_EQ(h, 4 * 22 + 6);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            g.TileCorner(column, row, x, y);
            float c = -1.0f, r = -1.0f;
            map->PixelsToTile(static_cast<float>(x + 16), static_cast<float>(y + 14), c, r);
            EXPECT_EQ(c, static_cast<float>(column)) << column << "," << row;
            EXPECT_EQ(r, static_cast<float>(row)) << column << "," << row;
        }
    }
}

TEST_F(IsometricMap, AHexagonalMapCanShiftItsEvenColumns)
{
    TmjMap source = SmallMap(false);
    source.tileHeight = 32;
    source.hexagonal = true;
    source.hexSideLength = 16;
    source.staggerX = true;
    source.staggerEven = true;
    Tilemap* map = Load(source);
    ASSERT_NE(map, nullptr);
    const Tilemap::StaggerGrid& g = map->Stagger();
    EXPECT_EQ(g.columnWidth, 24);
    EXPECT_EQ(g.rowHeight, 16);
    int32_t x = 0, y = 0;
    g.TileCorner(0, 0, x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 16) << "column 0 is even, so it is the one shifted down";
    g.TileCorner(1, 0, x, y);
    EXPECT_EQ(x, 24);
    EXPECT_EQ(y, 0);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            g.TileCorner(column, row, x, y);
            float c = -1.0f, r = -1.0f;
            map->PixelsToTile(static_cast<float>(x + 16), static_cast<float>(y + 16), c, r);
            EXPECT_EQ(c, static_cast<float>(column)) << column << "," << row;
            EXPECT_EQ(r, static_cast<float>(row)) << column << "," << row;
        }
    }
}
