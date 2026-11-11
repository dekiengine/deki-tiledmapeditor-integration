#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Tilemap.h"

namespace DekiTiledMap
{

/// Tiled's layer blend modes, one colour channel at a time, over a frame that
/// is opaque: the composition modes of Qt, which Tiled draws with, in their
/// 8-bit form. `dst` is the frame's channel, `src` the drawn pixel's channel
/// premultiplied by its alpha `sa`; all 0 to 255. Returns the new channel.
namespace TileBlend
{

inline int Div255(int x)
{
    return (x + (x >> 8) + 0x80) >> 8;
}

/// One mode, chosen at compile time, so a loop over a row holds no switch.
template <DBlendMode Mode>
inline uint8_t ChannelOf(int dst, int src, int sa)
{
    constexpr int da = 255;  // the frame is opaque
    constexpr DBlendMode mode = Mode;
    const int temp = src * (255 - da) + dst * (255 - sa);
    int out = dst;
    switch (mode)
    {
        case DBlendMode::Normal: out = src + Div255(dst * (255 - sa)); break;
        case DBlendMode::Add: out = src + dst; break;
        case DBlendMode::Multiply: out = Div255(src * dst + temp); break;
        case DBlendMode::Screen: out = src + dst - Div255(src * dst); break;
        case DBlendMode::Overlay:
            out = 2 * dst < da ? Div255(2 * src * dst + temp) : Div255(sa * da - 2 * (da - dst) * (sa - src) + temp);
            break;
        case DBlendMode::Darken:
        case DBlendMode::Lighten:
        {
            const int srcDa = src * da;
            const int dstSa = dst * sa;
            const bool srcWins = mode == DBlendMode::Darken ? srcDa < dstSa : srcDa > dstSa;
            out = Div255((srcWins ? srcDa : dstSa) + temp);
            break;
        }
        case DBlendMode::ColorDodge:
        {
            const int saDa = sa * da;
            const int dstSa = dst * sa;
            const int srcDa = src * da;
            if (srcDa + dstSa >= saDa)
            {
                out = Div255(saDa + temp);
            }
            else
            {
                out = Div255(255 * dstSa / (255 - 255 * src / sa) + temp);
            }
            break;
        }
        case DBlendMode::ColorBurn:
        {
            const int saDa = sa * da;
            const int dstSa = dst * sa;
            const int srcDa = src * da;
            if (src == 0 || srcDa + dstSa <= saDa)
            {
                out = Div255(temp);
            }
            else
            {
                out = Div255(sa * (srcDa + dstSa - saDa) / src + temp);
            }
            break;
        }
        case DBlendMode::HardLight:
            out = 2 * src < sa ? Div255(2 * src * dst + temp) : Div255(sa * da - 2 * (da - dst) * (sa - src) + temp);
            break;
        case DBlendMode::SoftLight:
        {
            const int src2 = src << 1;
            const int dstNp = (255 * dst) / da;
            const int t = temp * 255;
            if (src2 < sa)
            {
                out = (dst * (sa * 255 + (src2 - sa) * (255 - dstNp)) + t) / 65025;
            }
            else if (4 * dst <= da)
            {
                out = (dst * sa * 255 +
                       da * (src2 - sa) * ((((16 * dstNp - 12 * 255) * dstNp + 3 * 65025) * dstNp) / 65025) + t) /
                      65025;
            }
            else
            {
                const int root = static_cast<int>(std::sqrt(static_cast<double>(dstNp * 255)));
                out = (dst * sa * 255 + da * (src2 - sa) * (root - dstNp) + t) / 65025;
            }
            break;
        }
        case DBlendMode::Difference: out = src + dst - Div255(2 * std::min(src * da, dst * sa)); break;
        case DBlendMode::Exclusion: out = src + dst - Div255(2 * src * dst); break;
    }
    return static_cast<uint8_t>(std::clamp(out, 0, 255));
}

inline uint8_t Channel(DBlendMode mode, int dst, int src, int sa)
{
    switch (mode)
    {
        case DBlendMode::Normal: return ChannelOf<DBlendMode::Normal>(dst, src, sa);
        case DBlendMode::Add: return ChannelOf<DBlendMode::Add>(dst, src, sa);
        case DBlendMode::Multiply: return ChannelOf<DBlendMode::Multiply>(dst, src, sa);
        case DBlendMode::Screen: return ChannelOf<DBlendMode::Screen>(dst, src, sa);
        case DBlendMode::Overlay: return ChannelOf<DBlendMode::Overlay>(dst, src, sa);
        case DBlendMode::Darken: return ChannelOf<DBlendMode::Darken>(dst, src, sa);
        case DBlendMode::Lighten: return ChannelOf<DBlendMode::Lighten>(dst, src, sa);
        case DBlendMode::ColorDodge: return ChannelOf<DBlendMode::ColorDodge>(dst, src, sa);
        case DBlendMode::ColorBurn: return ChannelOf<DBlendMode::ColorBurn>(dst, src, sa);
        case DBlendMode::HardLight: return ChannelOf<DBlendMode::HardLight>(dst, src, sa);
        case DBlendMode::SoftLight: return ChannelOf<DBlendMode::SoftLight>(dst, src, sa);
        case DBlendMode::Difference: return ChannelOf<DBlendMode::Difference>(dst, src, sa);
        case DBlendMode::Exclusion: return ChannelOf<DBlendMode::Exclusion>(dst, src, sa);
    }
    return static_cast<uint8_t>(dst);
}

/// The alpha of each pixel of a row drawn twice, over black and over white
/// (0xAARRGGBB): over white it adds 255 times what it lets through.
inline void AlphaFromBlackAndWhite(const uint32_t* onBlack, const uint32_t* onWhite, uint8_t* alpha, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const uint32_t b = onBlack[i];
        const uint32_t w = onWhite[i];
        const int through = ((static_cast<int>((w >> 16) & 0xFFu) - static_cast<int>((b >> 16) & 0xFFu)) +
                             (static_cast<int>((w >> 8) & 0xFFu) - static_cast<int>((b >> 8) & 0xFFu)) +
                             (static_cast<int>(w & 0xFFu) - static_cast<int>(b & 0xFFu)) + 1) /
                            3;
        alpha[i] = static_cast<uint8_t>(std::clamp(255 - through, 0, 255));
    }
}

/// Blends a row of drawn pixels into a row of the frame, both 0xAARRGGBB.
/// `drawn` holds each pixel's colour times its alpha (as drawn over black),
/// `alpha` its alpha, or nullptr when every pixel is opaque.
template <DBlendMode Mode>
inline void BlendRow(uint32_t* frame, const uint32_t* drawn, const uint8_t* alpha, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const int sa = alpha ? alpha[i] : 255;
        if (sa == 0)
        {
            continue;
        }
        const uint32_t p = drawn[i];
        const int pr = std::min(static_cast<int>((p >> 16) & 0xFFu), sa);
        const int pg = std::min(static_cast<int>((p >> 8) & 0xFFu), sa);
        const int pb = std::min(static_cast<int>(p & 0xFFu), sa);
        const uint32_t d = frame[i];
        const int da = static_cast<int>(d >> 24);
        const uint32_t r = ChannelOf<Mode>(static_cast<int>((d >> 16) & 0xFFu), pr, sa);
        const uint32_t g = ChannelOf<Mode>(static_cast<int>((d >> 8) & 0xFFu), pg, sa);
        const uint32_t b = ChannelOf<Mode>(static_cast<int>(d & 0xFFu), pb, sa);
        const uint32_t a = static_cast<uint32_t>(sa + Div255(da * (255 - sa)));
        frame[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

}  // namespace TileBlend
}  // namespace DekiTiledMap
