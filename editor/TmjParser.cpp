#ifdef DEKI_EDITOR

#include "TmjParser.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <miniz.h>

#include "DekiLogSystem.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace DekiTilemap
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

// Decode a base64 string into bytes. Returns false on malformed input.
bool DecodeBase64(const std::string& in, std::vector<uint8_t>& out)
{
    static const int8_t lut[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    out.clear();
    out.reserve(in.size() * 3 / 4);
    int bits = 0, vacc = 0;
    for (unsigned char c : in)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') break;
        int8_t d = lut[c];
        if (d < 0) return false;
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

// Inflate a zlib-compressed buffer using miniz.
bool InflateZlib(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, size_t expected)
{
    out.resize(expected);
    mz_ulong dstLen = static_cast<mz_ulong>(expected);
    int r = mz_uncompress(out.data(), &dstLen, src, static_cast<mz_ulong>(srcLen));
    if (r != MZ_OK)
        return false;
    out.resize(dstLen);
    return true;
}

uint32_t ParseTiledColor(const std::string& s)
{
    // Tiled writes "#RRGGBB" or "#AARRGGBB". We store RGBA8.
    if (s.empty() || s[0] != '#') return 0xFF000000u;
    auto hex = [&](size_t i) -> uint32_t {
        char c = s[i];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
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

bool DecodeLayerPayload(const json& jdata, const json& jlayer,
                        size_t expectedTiles,
                        std::vector<uint32_t>& out, std::string& err)
{
    // CSV/JSON-array path (uncompressed): payload is a JSON array of ints.
    if (jdata.is_array())
    {
        out.reserve(jdata.size());
        for (const auto& v : jdata)
            out.push_back(v.get<uint32_t>());
        return true;
    }
    // Encoded string path. Honor "encoding" + "compression".
    std::string encoding = jlayer.value("encoding", "");
    std::string comp     = jlayer.value("compression", "");
    if (encoding == "csv" || encoding.empty())
    {
        // CSV body
        const std::string& s = jdata.get_ref<const std::string&>();
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            if (tok.empty()) continue;
            out.push_back(static_cast<uint32_t>(std::stoul(tok)));
        }
        return true;
    }
    if (encoding != "base64")
    {
        err = "unsupported layer encoding: " + encoding;
        return false;
    }
    if (!(comp.empty() || comp == "zlib"))
    {
        err = "unsupported layer compression: " + comp +
              " (only uncompressed and zlib are supported)";
        return false;
    }
    std::vector<uint8_t> decoded;
    if (!DecodeBase64(jdata.get_ref<const std::string&>(), decoded))
    {
        err = "malformed base64 in layer data";
        return false;
    }
    if (comp == "zlib")
    {
        std::vector<uint8_t> inflated;
        if (!InflateZlib(decoded.data(), decoded.size(), inflated, expectedTiles * 4))
        {
            err = "zlib inflate failed";
            return false;
        }
        decoded = std::move(inflated);
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

void ParseProperties(const json& jprops, std::vector<TmjPropertyValue>& out)
{
    if (!jprops.is_array()) return;
    for (const auto& p : jprops)
    {
        TmjPropertyValue v;
        v.name = p.value("name", "");
        v.type = p.value("type", "string");
        if (v.type == "int")        v.ivalue = p.value("value", 0);
        else if (v.type == "float") v.fvalue = p.value("value", 0.0f);
        else if (v.type == "bool")  v.bvalue = p.value("value", false);
        else                        v.svalue = p.value("value", std::string());
        out.push_back(std::move(v));
    }
}

} // namespace

bool ParseTmjMap(const std::string& tmjAbsPath, TmjMap& outMap, std::string& outError)
{
    std::string raw;
    if (!ReadFile(tmjAbsPath, raw, outError))
        return false;

    json j;
    try { j = json::parse(raw); }
    catch (const std::exception& e) { outError = std::string("json parse: ") + e.what(); return false; }

    outMap.width       = j.value("width",  0);
    outMap.height      = j.value("height", 0);
    outMap.tileWidth   = j.value("tilewidth",  0);
    outMap.tileHeight  = j.value("tileheight", 0);
    outMap.infinite    = j.value("infinite", false);
    outMap.chunkWidth  = j.value("editorsettings", json{}).value("chunksize", json{}).value("width", 16);
    outMap.chunkHeight = j.value("editorsettings", json{}).value("chunksize", json{}).value("height", 16);

    if (j.contains("backgroundcolor"))
        outMap.backgroundColor = ParseTiledColor(j["backgroundcolor"].get<std::string>());

    // Tilesets — external only (embedded rejected).
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
            r.source   = jt["source"].get<std::string>();
            outMap.tilesets.push_back(std::move(r));
        }
    }

    if (j.contains("layers") && j["layers"].is_array())
    {
        for (const auto& jl : j["layers"])
        {
            std::string type = jl.value("type", "");
            if (type == "tilelayer")
            {
                TmjLayer L;
                L.name    = jl.value("name", "");
                L.id      = jl.value("id",   0);
                L.visible = jl.value("visible", true);
                L.width   = jl.value("width",  outMap.width);
                L.height  = jl.value("height", outMap.height);

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
                        c.x      = jc.value("x", 0);
                        c.y      = jc.value("y", 0);
                        c.width  = jc.value("width",  outMap.chunkWidth);
                        c.height = jc.value("height", outMap.chunkHeight);
                        if (!DecodeLayerPayload(jc["data"], jl,
                                                static_cast<size_t>(c.width) * c.height,
                                                c.data, outError))
                            return false;
                        L.chunks.push_back(std::move(c));
                    }
                }
                else
                {
                    if (!jl.contains("data"))
                    {
                        outError = "finite tile layer missing 'data'";
                        return false;
                    }
                    if (!DecodeLayerPayload(jl["data"], jl,
                                            static_cast<size_t>(L.width) * L.height,
                                            L.data, outError))
                        return false;
                }

                outMap.tileLayers.push_back(std::move(L));
            }
            else if (type == "objectgroup")
            {
                TmjObjectLayer OL;
                OL.name    = jl.value("name", "");
                OL.id      = jl.value("id",   0);
                OL.visible = jl.value("visible", true);
                if (jl.contains("objects") && jl["objects"].is_array())
                {
                    for (const auto& jo : jl["objects"])
                    {
                        TmjObject o;
                        o.id       = jo.value("id", 0u);
                        o.name     = jo.value("name", "");
                        o.type     = jo.value("type", "");
                        o.x        = static_cast<int32_t>(jo.value("x", 0.0));
                        o.y        = static_cast<int32_t>(jo.value("y", 0.0));
                        o.width    = static_cast<int32_t>(jo.value("width",  0.0));
                        o.height   = static_cast<int32_t>(jo.value("height", 0.0));
                        o.rotation = jo.value("rotation", 0.0f);
                        o.gid      = jo.value("gid", 0u);
                        o.ellipse  = jo.value("ellipse", false);
                        if (jo.contains("polygon") && jo["polygon"].is_array())
                            for (const auto& p : jo["polygon"])
                                { o.polygonPoints.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                                  o.polygonPoints.push_back(static_cast<int32_t>(p.value("y", 0.0))); }
                        if (jo.contains("polyline") && jo["polyline"].is_array())
                            for (const auto& p : jo["polyline"])
                                { o.polylinePoints.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                                  o.polylinePoints.push_back(static_cast<int32_t>(p.value("y", 0.0))); }
                        if (jo.contains("properties"))
                            ParseProperties(jo["properties"], o.properties);
                        OL.objects.push_back(std::move(o));
                    }
                }
                outMap.objectLayers.push_back(std::move(OL));
            }
            // Group / image / other layer types: skipped silently in v1.
        }
    }

    return true;
}

bool ParseTsjTileset(const std::string& tsjAbsPath, TmjTileset& outTs, std::string& outError)
{
    std::string raw;
    if (!ReadFile(tsjAbsPath, raw, outError))
        return false;

    json j;
    try { j = json::parse(raw); }
    catch (const std::exception& e) { outError = std::string("json parse: ") + e.what(); return false; }

    outTs.name       = j.value("name", "");
    outTs.tileWidth  = j.value("tilewidth", 0);
    outTs.tileHeight = j.value("tileheight", 0);
    outTs.tileCount  = j.value("tilecount", 0);
    outTs.columns    = j.value("columns", 0);
    if (outTs.columns > 0 && outTs.tileCount > 0)
        outTs.rows = (outTs.tileCount + outTs.columns - 1) / outTs.columns;

    if (!j.contains("image"))
    {
        outError = "image-collection tilesets are not supported. Tileset must reference a single image.";
        return false;
    }
    outTs.imageRelative = j["image"].get<std::string>();

    if (j.contains("tiles") && j["tiles"].is_array())
    {
        for (const auto& jt : j["tiles"])
        {
            TmjTilesetTile t;
            t.id = jt.value("id", 0u);
            if (jt.contains("properties"))
                ParseProperties(jt["properties"], t.properties);
            if (jt.contains("animation") && jt["animation"].is_array())
            {
                for (const auto& jf : jt["animation"])
                {
                    TmjTilesetTile::Frame f;
                    f.tileId     = jf.value("tileid", 0u);
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
                    t.cw = static_cast<int32_t>(o.value("width",  0.0));
                    t.ch = static_cast<int32_t>(o.value("height", 0.0));
                    if (o.value("ellipse", false))
                        t.collisionShape = 1;     // DTileCollisionShape::Ellipse
                    else if (o.contains("polygon"))
                    {
                        t.collisionShape = 2;     // Polygon
                        for (const auto& p : o["polygon"])
                            { t.collisionPolygon.push_back(static_cast<int32_t>(p.value("x", 0.0)));
                              t.collisionPolygon.push_back(static_cast<int32_t>(p.value("y", 0.0))); }
                    }
                    else
                        t.collisionShape = 0;     // Rect
                }
            }
            outTs.tiles.push_back(std::move(t));
        }
    }

    return true;
}

} // namespace DekiTilemap

#endif // DEKI_EDITOR
