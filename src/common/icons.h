#pragma once
#include <cstdint>
#include <string>
#include <vector>

// the game's controller button icons, or others drawn in their place (icons.cpp)
namespace icons {

enum class Style : uint8_t { Original, Keyboard, Xbox, PlayStation4, PlayStation5, Switch };

// each poll of port 1: the prompts for the device used last
void Update(Style style, bool log = true);

// the keys were bound again: the keyboard's prompts are drawn again with them at the next Update
void KeysChanged();

// Keys of a prompt's text drawn as pictures while the keyboard is in use. The text names each key by a null icon (code
// '8'); the game's bind of that icon's texture takes the next picture of the queue set before the text is drawn.
bool HasKeyPicture(const std::string& label);
void BeginKeyPictures(const std::vector<std::string>& keys);  // the keys of the text about to be drawn, in order
bool KeyPictureWanted(uint32_t texture_id);                   // the texture bound is the null icon, with a key left
void PutKeyPicture();                                         // the next key's picture into it (flush queued quads first)
bool EndKeyPictures();                                        // the text is drawn; true if the icon holds a picture
void GiveBackNullIcon();                                      // (after flushing) the icon's own picture back

// the prompts last set, and a style's name for the log
Style Current();
const char* Name(Style style);

}  // namespace icons
