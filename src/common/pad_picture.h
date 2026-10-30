#pragma once
#include <cstdint>
#include <vector>

#include "common/icons.h"

// pictures of the controllers for the Controls screen, in place of the game's Xbox one (pad_picture.cpp)
namespace pad_picture {

// the screen draws its picture 229 by 144 and the halo behind it 1.05 as wide and 152 tall, about the same centre
constexpr float kWidth = 229.0f, kHeight = 144.0f, kGlowWidth = kWidth * 1.05f, kGlowHeight = 152.0f;

// the controller of an Xbox, PlayStation or Switch style, as ARGB rows top down stretched to w by h; empty for others
std::vector<uint32_t> Picture(icons::Style style, uint32_t w, uint32_t h);

// its outline grown into a halo, grey with alpha like the game's, stretched to w by h
std::vector<uint32_t> Glow(icons::Style style, uint32_t w, uint32_t h);

}  // namespace pad_picture
