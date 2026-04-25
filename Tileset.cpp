#include "Tileset.h"

#include <cstdio>
#include <cstring>

#include "DekiLogSystem.h"
#include "assets/AssetManager.h"

namespace DekiTilemap
{

Tileset::~Tileset() = default;

Tileset* Tileset::Load(const char* dtilesetPath)
{
    if (!dtilesetPath)
        return nullptr;

    FILE* f = std::fopen(dtilesetPath, "rb");
    if (!f)
    {
        DEKI_LOG_ERROR("Tileset::Load: cannot open '%s'", dtilesetPath);
        return nullptr;
    }

    DTilesetHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1)
    {
        std::fclose(f);
        DEKI_LOG_ERROR("Tileset::Load: short read on header for '%s'", dtilesetPath);
        return nullptr;
    }
    if (std::memcmp(hdr.magic, "DTS1", 4) != 0 || hdr.version != 1)
    {
        std::fclose(f);
        DEKI_LOG_ERROR("Tileset::Load: bad magic/version in '%s'", dtilesetPath);
        return nullptr;
    }

    auto* ts = new Tileset();
    ts->m_header = hdr;
    ts->m_atlas.guid = std::string(hdr.atlasGuid, strnlen(hdr.atlasGuid, 36));

    if (hdr.animCount > 0)
    {
        ts->m_anims.resize(hdr.animCount);
        std::fseek(f, static_cast<long>(hdr.animTableOffset), SEEK_SET);
        std::fread(ts->m_anims.data(), sizeof(DTileAnimation), hdr.animCount, f);

        // Pull the frames blob: we trust the baker to lay frames contiguously
        // immediately after the animation table.
        uint32_t totalFrames = 0;
        for (const auto& a : ts->m_anims) totalFrames += a.frameCount;
        if (totalFrames > 0)
        {
            ts->m_animFrames.resize(totalFrames);
            uint32_t firstOffset = ts->m_anims.front().frameOffset;
            std::fseek(f, static_cast<long>(firstOffset), SEEK_SET);
            std::fread(ts->m_animFrames.data(), sizeof(DTileAnimationFrame), totalFrames, f);
        }
    }

    if (hdr.collisionCount > 0)
    {
        ts->m_collisions.resize(hdr.collisionCount);
        std::fseek(f, static_cast<long>(hdr.collisionTableOffset), SEEK_SET);
        std::fread(ts->m_collisions.data(), sizeof(DTileCollision), hdr.collisionCount, f);
    }

    std::fclose(f);
    return ts;
}

Sprite* Tileset::Atlas() const
{
    return m_atlas.Get();
}

void Tileset::GetTileRect(uint32_t localId, int& x, int& y, int& w, int& h) const
{
    const uint32_t cols = m_header.columns ? m_header.columns : 1;
    x = static_cast<int>((localId % cols) * m_header.tileWidth);
    y = static_cast<int>((localId / cols) * m_header.tileHeight);
    w = m_header.tileWidth;
    h = m_header.tileHeight;
}

const DTileAnimation* Tileset::GetAnimation(uint32_t localId) const
{
    for (const auto& a : m_anims)
        if (a.localId == localId) return &a;
    return nullptr;
}

const DTileCollision* Tileset::GetCollision(uint32_t localId) const
{
    for (const auto& c : m_collisions)
        if (c.localId == localId) return &c;
    return nullptr;
}

const DTileAnimationFrame* Tileset::GetAnimationFrames(const DTileAnimation& a) const
{
    if (m_animFrames.empty() || m_anims.empty()) return nullptr;
    const uint32_t base = m_anims.front().frameOffset;
    if (a.frameOffset < base) return nullptr;
    const uint32_t idx = (a.frameOffset - base) / sizeof(DTileAnimationFrame);
    if (idx + a.frameCount > m_animFrames.size()) return nullptr;
    return &m_animFrames[idx];
}

// REGISTER_ASSET_TYPE concatenates the type name into an identifier, so it
// can't accept a qualified name. Call inside the namespace.
REGISTER_ASSET_TYPE(Tileset, Tileset::Load)

} // namespace DekiTilemap
