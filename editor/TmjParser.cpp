#ifdef DEKI_EDITOR

#include <algorithm>
#include <functional>

#include <deki/LogSystem.h>

#include "TmjParser.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <deki/LogSystem.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiTiledMap
{

namespace
{

bool ReadFile(const std::string& path, std::string& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Decodes a base64 string. Returns false on malformed input.
bool DecodeBase64(const std::string& in, std::vector<uint8_t>& out)
{
    static const int8_t kLut[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55,
        56, 57, 58, 59, 60, 61, -1, -1, -1, 0,  -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32,
        33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
    out.clear();
    out.reserve(in.size() * 3 / 4);
    int bits = 0, vacc = 0;
    for (unsigned char c : in)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
        {
            continue;
        }
        if (c == '=')
        {
            break;
        }
        int8_t d = kLut[c];
        if (d < 0)
        {
            return false;
        }
        vacc = (vacc << 6) | d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((vacc >> bits) & 0xFF));
        }
    }
    return true;
}

uint32_t ParseTiledColor(const std::string& s)
{
    // Tiled writes "#RRGGBB" or "#AARRGGBB". Returned as RGBA8, R in the low
    // byte.
    if (s.empty() || s[0] != '#')
    {
        return 0xFF000000u;
    }
    auto hex = [&](size_t i) -> uint32_t
    {
        char c = s[i];
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F')
        {
            return 10 + (c - 'A');
        }
        return 0;
    };
    if (s.size() == 7)
    {
        uint32_t r = (hex(1) << 4) | hex(2);
        uint32_t g = (hex(3) << 4) | hex(4);
        uint32_t b = (hex(5) << 4) | hex(6);
        return (0xFFu << 24) | (b << 16) | (g << 8) | r;
    }
    if (s.size() == 9)
    {
        uint32_t a = (hex(1) << 4) | hex(2);
        uint32_t r = (hex(3) << 4) | hex(4);
        uint32_t g = (hex(5) << 4) | hex(6);
        uint32_t b = (hex(7) << 4) | hex(8);
        return (a << 24) | (b << 16) | (g << 8) | r;
    }
    return 0xFF000000u;
}

bool DecodeLayerPayload(const json& jdata, const json& jlayer, size_t expectedTiles, std::vector<uint32_t>& out,
                        std::string& err)
{
    // Uncompressed: a JSON array of ints.
    if (jdata.is_array())
    {
        out.reserve(jdata.size());
        for (const auto& v : jdata)
        {
            out.push_back(v.get<uint32_t>());
        }
        return true;
    }
    // A string, decoded by "encoding" and "compression".
    std::string encoding = jlayer.value("encoding", "");
    std::string comp = jlayer.value("compression", "");
    if (encoding == "csv" || encoding.empty())
    {
        const std::string& s = jdata.get_ref<const std::string&>();
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            if (tok.empty())
            {
                continue;
            }
            out.push_back(static_cast<uint32_t>(std::stoul(tok)));
        }
        return true;
    }
    if (encoding != "base64")
    {
        err = "unsupported layer encoding: " + encoding;
        return false;
    }
    if (!comp.empty())
    {
        err = "unsupported layer compression: " + comp +
              " (v1 supports uncompressed CSV / base64 only — re-export from "
              "Tiled with 'Tile Layer Format: Base64 (uncompressed)' or CSV)";
        return false;
    }
    std::vector<uint8_t> decoded;
    if (!DecodeBase64(jdata.get_ref<const std::string&>(), decoded))
    {
        err = "malformed base64 in layer data";
        return false;
    }
    if (decoded.size() != expectedTiles * 4)
    {
        err = "layer payload size mismatch";
        return false;
    }
    out.resize(expectedTiles);
    std::memcpy(out.data(), decoded.data(), expectedTiles * 4);
    return true;
}

uint32_t ObjectAlignmentFromName(const std::string& name)
{
    static const char* const kNames[] = { "topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright" };
    for (uint32_t i = 0; i < 9; ++i)
    {
        if (name == kNames[i])
        {
            return i + 1;
        }
    }
    return 0;
}

// Tiled's name for a layer's blend mode (its "mode"), as a DBlendMode. An
// unknown one is drawn Normal, and said once at bake time.
uint8_t BlendModeFromName(const std::string& mode, const std::string& layerName)
{
    static const char* const kNames[] = { "normal",     "add",        "multiply",   "screen",     "overlay",
                                          "darken",     "lighten",    "color-dodge", "color-burn", "hard-light",
                                          "soft-light", "difference", "exclusion" };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
    {
        if (mode == kNames[i])
        {
            return static_cast<uint8_t>(i);
        }
    }
    DEKI_LOG_WARNING("Tiled layer '%s': blend mode '%s' is not known, so it is drawn Normal.", layerName.c_str(),
                     mode.c_str());
    return 0;
}

// Multiplies two RGBA8 colours, channel by channel.
uint32_t MultiplyColors(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const uint32_t ca = (a >> shift) & 0xFFu;
        const uint32_t cb = (b >> shift) & 0xFFu;
        out |= ((ca * cb + 127u) / 255u) << shift;
    }
    return out;
}

// A layer's own look, on top of its group's: opacity and tint multiply,
// offsets add and parallax factors multiply, as in Tiled.
TmjLayerLook LookOf(const json& jl, const TmjLayerLook& parent)
{
    TmjLayerLook look = parent;
    look.visible = parent.visible && jl.value("visible", true);
    look.opacity = parent.opacity * jl.value("opacity", 1.0f);
    if (jl.contains("tintcolor") && jl["tintcolor"].is_string())
    {
        look.tint = MultiplyColors(parent.tint, ParseTiledColor(jl["tintcolor"].get<std::string>()));
    }
    look.offsetX = parent.offsetX + jl.value("offsetx", 0.0f);
    look.offsetY = parent.offsetY + jl.value("offsety", 0.0f);
    look.parallaxX = parent.parallaxX * jl.value("parallaxx", 1.0f);
    look.parallaxY = parent.parallaxY * jl.value("parallaxy", 1.0f);
    // A layer's own mode, or else its group's.
    const std::string mode = jl.value("mode", std::string("normal"));
    if (mode != "normal")
    {
        look.blendMode = BlendModeFromName(mode, jl.value("name", std::string()));
    }
    return look;
}

void ParseProperties(const json& jprops, std::vector<TmjPropertyValue>& out)
{
    if (!jprops.is_array())
    {
        return;
    }
    for (const auto& p : jprops)
    {
        TmjPropertyValue v;
        v.name = p.value("name", "");
        v.type = p.value("type", "string");
        if (v.type == "int")
        {
            v.ivalue = p.value("value", 0);
        }
        else if (v.type == "float")
        {
            v.fvalue = p.value("value", 0.0f);
        }
        else if (v.type == "bool")
        {
            v.bvalue = p.value("value", false);
        }
        else
        {
            v.svalue = p.value("value", std::string());
        }
        out.push_back(std::move(v));
    }
}

}  // namespace

bool ParseTmjMap(const std::string& tmjAbsPath, TmjMap& outMap, std::string& outError)
{
    std::string raw;
    if (!ReadFile(tmjAbsPath, raw, outError))
    {
        return false;
    }

    json j;
    try
    {
        j = json::parse(raw);
    }
    catch (const std::exception& e)
    {
        outError = std::string("json parse: ") + e.what();
        return false;
    }

    outMap.width = j.value("width", 0);
    outMap.height = j.value("height", 0);
    outMap.tileWidth = j.value("tilewidth", 0);
    outMap.tileHeight = j.value("tileheight", 0);
    outMap.infinite = j.value("infinite", false);

    // A layout the renderer does not know would be drawn as something else.
    const std::string orientation = j.value("orientation", std::string("orthogonal"));
    if (orientation != "orthogonal" && orientation != "isometric" && orientation != "staggered" &&
        orientation != "hexagonal")
    {
        outError = "'" + orientation +
                   "' maps are not supported. Use orthogonal, isometric, staggered or hexagonal (Map > Map Properties "
                   "> Orientation).";
        return false;
    }
    outMap.isometric = orientation == "isometric";
    outMap.staggered = orientation == "staggered";
    outMap.hexagonal = orientation == "hexagonal";
    if (outMap.staggered || outMap.hexagonal)
    {
        outMap.staggerX = j.value("staggeraxis", std::string("y")) == "x";
        outMap.staggerEven = j.value("staggerindex", std::string("odd")) == "even";
        outMap.hexSideLength = outMap.hexagonal ? std::max(0, j.value("hexsidelength", 0)) : 0;
    }

    // json::object(), not json{}, as the default: a default json is null, and
    // .value() on null throws type_error.302.
    outMap.chunkWidth = j.value("editorsettings", json::object()).value("chunksize", json::object()).value("width", 16);
    outMap.chunkHeight =
        j.value("editorsettings", json::object()).value("chunksize", json::object()).value("height", 16);

    if (j.contains("backgroundcolor"))
    {
        outMap.backgroundColor = ParseTiledColor(j["backgroundcolor"].get<std::string>());
    }
    outMap.parallaxOriginX = j.value("parallaxoriginx", 0.0f);
    outMap.parallaxOriginY = j.value("parallaxoriginy", 0.0f);

    // Tilesets: external only; embedded ones are rejected.
    if (j.contains("tilesets") && j["tilesets"].is_array())
    {
        for (const auto& jt : j["tilesets"])
        {
            if (!jt.contains("source"))
            {
                outError = "embedded tilesets are not supported. Tiled tileset must reference an external .tsj.";
                return false;
            }
            TmjTilesetRef r;
            r.firstGid = jt.value("firstgid", 1u);
            r.source = jt["source"].get<std::string>();
            outMap.tilesets.push_back(std::move(r));
        }
    }

    // Groups are flattened in order: what Tiled shows is their layers, and a
    // group's visibility, opacity, tint, offset and parallax apply to all of them.
    std::function<bool(const json&, const TmjLayerLook&)> parseLayers = [&](const json& layers,
                                                                          const TmjLayerLook& parent) -> bool
    {
        for (const auto& jl : layers)
        {
            std::string type = jl.value("type", "");
            TmjLayerLook look = LookOf(jl, parent);
            const bool layerVisible = look.visible;
            if (type == "objectgroup" && look.blendMode != 0)
            {
                // Spawned objects are drawn by the scene's sprites, one by one.
                DEKI_LOG_WARNING("Tiled layer '%s': an object layer's blend mode is not drawn by Deki.",
                                 jl.value("name", std::string()).c_str());
            }
            if (type == "tilelayer")
            {
                look.kind = TmjLayerLook::Kind::Tile;
                look.index = static_cast<uint32_t>(outMap.tileLayers.size());
                outMap.layers.push_back(look);
                TmjLayer l;
                l.name = jl.value("name", "");
                l.id = jl.value("id", 0);
                l.visible = layerVisible;
                l.width = jl.value("width", outMap.width);
                l.height = jl.value("height", outMap.height);

                if (outMap.infinite)
                {
                    if (!jl.contains("chunks") || !jl["chunks"].is_array())
                    {
                        outError = "infinite map layer missing 'chunks' array";
                        return false;
                    }
                    for (const auto& jc : jl["chunks"])
                    {
                        TmjChunk c;
                        c.x = jc.value("x", 0);
                        c.y = jc.value("y", 0);
                        c.width = jc.value("width", outMap.chunkWidth);
                        c.height = jc.value("height", outMap.chunkHeight);
                        if (!DecodeLayerPayload(jc["data"], jl, static_cast<size_t>(c.width) * c.height, c.data,
                                                outError))
                        {
                            return false;
                        }
                        l.chunks.push_back(std::move(c));
                    }
                }
                else
                {
                    if (!jl.contains("data"))
                    {
                        outError = "finite tile layer missing 'data'";
                        return false;
                    }
                    if (!DecodeLayerPayload(jl["data"], jl, static_cast<size_t>(l.width) * l.height, l.data, outError))
                    {
                        return false;
                    }
                }

                outMap.tileLayers.push_back(std::move(l));
            }
            else if (type == "objectgroup")
            {
                look.kind = TmjLayerLook::Kind::Object;
                look.index = static_cast<uint32_t>(outMap.objectLayers.size());
                outMap.layers.push_back(look);
                TmjObjectLayer ol;
                ol.name = jl.value("name", "");
                ol.id = jl.value("id", 0);
                ol.visible = layerVisible;
                if (jl.contains("objects") && jl["objects"].is_array())
                {
                    for (const auto& jo : jl["objects"])
                    {
                        TmjObject o;
                        o.id = jo.value("id", 0u);
                        o.name = jo.value("name", "");
                        o.type = jo.value("type", "");
                        o.x = jo.value("x", 0.0f);
                        o.y = jo.value("y", 0.0f);
                        o.width = jo.value("width", 0.0f);
                        o.height = jo.value("height", 0.0f);
                        o.rotation = jo.value("rotation", 0.0f);
                        o.gid = jo.value("gid", 0u);
                        o.ellipse = jo.value("ellipse", false);
                        if (jo.contains("polygon") && jo["polygon"].is_array())
                        {
                            for (const auto& p : jo["polygon"])
                            {
                                o.polygonPoints.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                                o.polygonPoints.push_back(static_cast<int32_t>(p.value("y", 0.0)));
                            }
                        }
                        if (jo.contains("polyline") && jo["polyline"].is_array())
                        {
                            for (const auto& p : jo["polyline"])
                            {
                                o.polylinePoints.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                                o.polylinePoints.push_back(static_cast<int32_t>(p.value("y", 0.0)));
                            }
                        }
                        if (jo.contains("properties"))
                        {
                            ParseProperties(jo["properties"], o.properties);
                        }
                        // Tiled does not draw a hidden tile object. One that
                        // spawns a scene, or any other object, is data and stays.
                        const bool spawnsScene = std::any_of(o.properties.begin(), o.properties.end(),
                                                             [](const TmjPropertyValue& p) { return p.name == "scene_guid"; });
                        if (o.gid != 0 && !spawnsScene && !(layerVisible && jo.value("visible", true)))
                        {
                            continue;
                        }
                        ol.objects.push_back(std::move(o));
                    }
                }
                outMap.objectLayers.push_back(std::move(ol));
            }
            else if (type == "imagelayer")
            {
                look.kind = TmjLayerLook::Kind::Image;
                look.index = outMap.imageLayerCount++;
                look.imageRelative = jl.value("image", std::string());
                look.repeatX = jl.value("repeatx", false);
                look.repeatY = jl.value("repeaty", false);
                outMap.layers.push_back(look);
            }
            else if (type == "group" && jl.contains("layers") && jl["layers"].is_array())
            {
                if (!parseLayers(jl["layers"], look))
                {
                    return false;
                }
            }
        }
        return true;
    };

    if (j.contains("layers") && j["layers"].is_array() && !parseLayers(j["layers"], TmjLayerLook{}))
    {
        return false;
    }

    return true;
}

bool ParseTsjTileset(const std::string& tsjAbsPath, TmjTileset& outTs, std::string& outError)
{
    std::string raw;
    if (!ReadFile(tsjAbsPath, raw, outError))
    {
        return false;
    }

    json j;
    try
    {
        j = json::parse(raw);
    }
    catch (const std::exception& e)
    {
        outError = std::string("json parse: ") + e.what();
        return false;
    }

    outTs.name = j.value("name", "");
    outTs.tileWidth = j.value("tilewidth", 0);
    outTs.tileHeight = j.value("tileheight", 0);
    outTs.margin = j.value("margin", 0);
    outTs.spacing = j.value("spacing", 0);
    if (j.contains("tileoffset"))
    {
        outTs.tileOffsetX = j["tileoffset"].value("x", 0);
        outTs.tileOffsetY = j["tileoffset"].value("y", 0);
    }
    outTs.objectAlignment = ObjectAlignmentFromName(j.value("objectalignment", std::string()));
    outTs.renderToGrid = j.value("tilerendersize", std::string("tile")) == "grid";
    outTs.preserveAspect = j.value("fillmode", std::string("stretch")) == "preserve-aspect-fit";
    outTs.tileCount = j.value("tilecount", 0);
    outTs.columns = j.value("columns", 0);
    if (outTs.columns > 0 && outTs.tileCount > 0)
    {
        outTs.rows = (outTs.tileCount + outTs.columns - 1) / outTs.columns;
    }

    if (!j.contains("image"))
    {
        outError = "image-collection tilesets are not supported. Tileset must reference a single image.";
        return false;
    }
    outTs.imageRelative = j["image"].get<std::string>();

    if (j.contains("transparentcolor"))
    {
        outTs.transparentColor = ParseTiledColor(j["transparentcolor"].get<std::string>());
        outTs.hasTransparentColor = true;
    }

    if (j.contains("tiles") && j["tiles"].is_array())
    {
        for (const auto& jt : j["tiles"])
        {
            TmjTilesetTile t;
            t.id = jt.value("id", 0u);
            if (jt.contains("properties"))
            {
                ParseProperties(jt["properties"], t.properties);
            }
            if (jt.contains("animation") && jt["animation"].is_array())
            {
                for (const auto& jf : jt["animation"])
                {
                    TmjTilesetTile::Frame f;
                    f.tileId = jf.value("tileid", 0u);
                    f.durationMs = jf.value("duration", 0u);
                    t.animation.push_back(f);
                }
            }
            if (jt.contains("objectgroup"))
            {
                const auto& og = jt["objectgroup"];
                if (og.contains("objects") && og["objects"].is_array() && !og["objects"].empty())
                {
                    const auto& o = og["objects"][0];
                    t.hasCollision = true;
                    t.cx = static_cast<int32_t>(o.value("x", 0.0));
                    t.cy = static_cast<int32_t>(o.value("y", 0.0));
                    t.cw = static_cast<int32_t>(o.value("width", 0.0));
                    t.ch = static_cast<int32_t>(o.value("height", 0.0));
                    if (o.value("ellipse", false))
                    {
                        t.collisionShape = 1;  // DTileCollisionShape::Ellipse
                    }
                    else if (o.contains("polygon"))
                    {
                        t.collisionShape = 2;  // Polygon
                        for (const auto& p : o["polygon"])
                        {
                            t.collisionPolygon.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                            t.collisionPolygon.push_back(static_cast<int32_t>(p.value("y", 0.0)));
                        }
                    }
                    else
                    {
                        t.collisionShape = 0;  // Rect
                    }
                }
            }
            outTs.tiles.push_back(std::move(t));
        }
    }

    return true;
}

}  // namespace DekiTiledMap

#endif  // DEKI_EDITOR
