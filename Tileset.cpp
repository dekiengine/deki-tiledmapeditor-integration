#include "Tileset.h"

#include <cstring>

#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>
#include <deki/providers/FileSystem.h>

namespace DekiTiledMap
{

Tileset::~Tileset() = default;

Tileset* Tileset::Load(const char* dtilesetPath)
{
    if (!dtilesetPath)
        return nullptr;

    // Through the engine filesystem, never stdio: the asset manager prefixes
    // the cache directory, which is the "S:/" mount on a device and in the
    // simulator, and only IFileSystem resolves that prefix. As std::fopen this
    // loaded in the editor and failed everywhere else.
    Deki::IFileSystem* fs = Deki::FileSystem::GetFileSystemForPath(dtilesetPath);
    if (!fs)
    {
        DEKI_LOG_ERROR("Tileset::Load: no filesystem provider available");
        return nullptr;
    }

    Deki::IFileSystem::FileHandle f =
        fs->OpenFile(dtilesetPath, Deki::IFileSystem::OpenMode::READ_BINARY);
    if (!f)
    {
        DEKI_LOG_ERROR("Tileset::Load: cannot open '%s'", dtilesetPath);
        return nullptr;
    }

    DTilesetHeader hdr{};
    if (fs->ReadFile(f, &hdr, sizeof(hdr)) != sizeof(hdr))
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tileset::Load: short read on header for '%s'", dtilesetPath);
        return nullptr;
    }
    if (std::memcmp(hdr.magic, "DTS1", 4) != 0 || hdr.version != 1)
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tileset::Load: bad magic/version in '%s'", dtilesetPath);
        return nullptr;
    }

    // Counts and offsets come from the file: each table is checked against
    // its size, in 64 bits, as Tilemap::Load does. A huge count made resize()
    // abort on the device, and the frame total could wrap.
    const long fileSizeL = fs->GetFileSize(f);
    const uint64_t fileBytes = fileSizeL > 0 ? static_cast<uint64_t>(fileSizeL) : 0;
    auto inFile = [fileBytes](uint64_t offset, uint64_t count, uint64_t elem)
    { return offset <= fileBytes && count <= (fileBytes - offset) / elem; };
    auto readAt = [&](uint64_t offset, void* into, uint64_t bytes)
    {
        fs->SeekFile(f, static_cast<long>(offset), Deki::IFileSystem::SeekOrigin::BEGIN);
        return fs->ReadFile(f, into, static_cast<size_t>(bytes)) == static_cast<size_t>(bytes);
    };
    auto damaged = [&](const char* what) -> Tileset*
    {
        fs->CloseFile(f);
        DEKI_LOG_ERROR("Tileset::Load: '%s' is damaged (%s)", dtilesetPath, what);
        return nullptr;
    };
    if (!inFile(hdr.animTableOffset, hdr.animCount, sizeof(DTileAnimation)) ||
        !inFile(hdr.collisionTableOffset, hdr.collisionCount, sizeof(DTileCollision)))
        return damaged("a table runs past the end of the file");

    auto* ts = new Tileset();
    ts->m_MHeader = hdr;
    ts->m_MAtlas.guid = std::string(hdr.atlasGuid, strnlen(hdr.atlasGuid, 36));
    auto fail = [&](const char* what) -> Tileset*
    {
        delete ts;
        return damaged(what);
    };

    if (hdr.animCount > 0)
    {
        ts->m_MAnims.resize(hdr.animCount);
        if (!readAt(hdr.animTableOffset, ts->m_MAnims.data(), uint64_t(sizeof(DTileAnimation)) * hdr.animCount))
            return fail("short animation table");

        // Pull the frames blob: we trust the baker to lay frames contiguously
        // immediately after the animation table.
        uint64_t totalFrames = 0;
        for (const auto& a : ts->m_MAnims) totalFrames += a.frameCount;
        const uint32_t firstOffset = ts->m_MAnims.front().frameOffset;
        if (!inFile(firstOffset, totalFrames, sizeof(DTileAnimationFrame)))
            return fail("animation frames run past the end of the file");
        if (totalFrames > 0)
        {
            ts->m_animFrames.resize(static_cast<size_t>(totalFrames));
            if (!readAt(firstOffset, ts->m_animFrames.data(), sizeof(DTileAnimationFrame) * totalFrames))
                return fail("short animation frames");
        }
    }

    if (hdr.collisionCount > 0)
    {
        ts->m_MCollisions.resize(hdr.collisionCount);
        if (!readAt(hdr.collisionTableOffset, ts->m_MCollisions.data(),
                    uint64_t(sizeof(DTileCollision)) * hdr.collisionCount))
            return fail("short collision table");
    }

    fs->CloseFile(f);
    return ts;
}

Deki2D::Sprite* Tileset::Atlas() const
{
    return m_MAtlas.Get();
}

void Tileset::GetTileRect(uint32_t localId, int& x, int& y, int& w, int& h) const
{
    const uint32_t cols = m_MHeader.columns ? m_MHeader.columns : 1;
    x = static_cast<int>((localId % cols) * m_MHeader.tileWidth);
    y = static_cast<int>((localId / cols) * m_MHeader.tileHeight);
    w = m_MHeader.tileWidth;
    h = m_MHeader.tileHeight;
}

const DTileAnimation* Tileset::GetAnimation(uint32_t localId) const
{
    for (const auto& a : m_MAnims)
        if (a.localId == localId) return &a;
    return nullptr;
}

const DTileCollision* Tileset::GetCollision(uint32_t localId) const
{
    for (const auto& c : m_MCollisions)
        if (c.localId == localId) return &c;
    return nullptr;
}

const DTileAnimationFrame* Tileset::GetAnimationFrames(const DTileAnimation& a) const
{
    if (m_animFrames.empty() || m_MAnims.empty()) return nullptr;
    const uint32_t base = m_MAnims.front().frameOffset;
    if (a.frameOffset < base) return nullptr;
    const uint32_t idx = (a.frameOffset - base) / sizeof(DTileAnimationFrame);
    if (uint64_t(idx) + a.frameCount > m_animFrames.size()) return nullptr;
    return &m_animFrames[idx];
}

// REGISTER_ASSET_TYPE concatenates the type name into an identifier, so it
// can't accept a qualified name. Call inside the namespace.
REGISTER_ASSET_TYPE(Tileset, Tileset::Load)

} // namespace DekiTiledMap
