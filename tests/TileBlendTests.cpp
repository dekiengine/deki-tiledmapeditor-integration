// Tiled's layer blend modes, channel by channel, as Qt draws them.

#include <gtest/gtest.h>

#include "TileBlend.h"

using DekiTiledMap::DBlendMode;
using DekiTiledMap::TileBlend::Channel;

TEST(TileBlend, OpaqueSourcesFollowTheModesFormula)
{
    EXPECT_EQ(Channel(DBlendMode::Normal, 50, 200, 255), 200);
    EXPECT_EQ(Channel(DBlendMode::Multiply, 200, 128, 255), 100);
    EXPECT_EQ(Channel(DBlendMode::Screen, 200, 128, 255), 228);
    EXPECT_EQ(Channel(DBlendMode::Add, 200, 100, 255), 255) << "Add stops at white";
    EXPECT_EQ(Channel(DBlendMode::Darken, 200, 128, 255), 128);
    EXPECT_EQ(Channel(DBlendMode::Lighten, 200, 128, 255), 200);
    EXPECT_EQ(Channel(DBlendMode::Difference, 200, 128, 255), 72);
}

TEST(TileBlend, HalfSeeThroughBlendsHalfway)
{
    // Colour 128 at alpha 128 is 64 premultiplied: half of 200 and half of
    // 200 x 128 / 255.
    EXPECT_EQ(Channel(DBlendMode::Multiply, 200, 64, 128), 150);
}

TEST(TileBlend, AClearPixelLeavesTheFrameInEveryMode)
{
    for (int mode = 0; mode <= static_cast<int>(DBlendMode::Exclusion); ++mode)
    {
        for (int dst : { 0, 37, 128, 255 })
        {
            EXPECT_EQ(Channel(static_cast<DBlendMode>(mode), dst, 0, 0), dst) << "mode " << mode;
        }
    }
}

TEST(TileBlend, WhiteDodgeAndBlackBurnDoNotDivideByZero)
{
    EXPECT_EQ(Channel(DBlendMode::ColorDodge, 100, 255, 255), 255);
    EXPECT_EQ(Channel(DBlendMode::ColorBurn, 100, 0, 255), 0);
}
