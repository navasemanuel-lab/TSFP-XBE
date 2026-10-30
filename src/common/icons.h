#pragma once
#include <cstdint>

// the game's controller button icons, or others drawn in their place (icons.cpp)
namespace icons {

enum class Style : uint8_t { Original, Keyboard, Xbox, PlayStation4, PlayStation5, Switch };

// each poll of port 1: the prompts for the device used last
void Update(Style style);

// the keys were bound again: the keyboard's prompts are drawn again with them at the next Update
void KeysChanged();

// the prompts last set, and a style's name for the log
Style Current();
const char* Name(Style style);

}  // namespace icons
