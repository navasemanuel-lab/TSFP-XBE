#pragma once
#include <cstddef>
#include <string>

namespace input {
struct Pad;
}

// controllers and the keyboard as the console's pads (controls.cpp)
namespace controls {

enum class Button { A, B, X, Y, Black, White, Start, Back };

void Pad(int port, input::Pad& pad);  // GameInfo::pad
bool KeyboardActive();                // the keyboard and mouse were used last, not a controller
int TakeWeapon();                     // the weapon key pressed just now (its inventory place, 1 to 10), else 0
const char* KeyLabel(Button b);       // the key a button's prompts show
// the keys a prompt's text code (0x0B then this) names for the controls drawn as text, not as a button icon: the
// triggers, D-pad, sticks and their clicks, as "[Q/V]"; empty for any other code
std::string PromptKeys(char code);
void LoadKeys();                      // the key bindings from Options::keys, filling in the defaults
const char* ActionIds(size_t i);      // the bindable actions' names, nullptr past the last
const char* ActionDefaults(size_t i);  // an action's default keys
std::string KeyName(int vk);          // a key's name in the settings file, empty for one that cannot be bound
// a binding changed (the settings panel): applied, kept in Options::keys and saved at the next poll
void SetKeys(const std::string& id, const std::string& keys);

}  // namespace controls
