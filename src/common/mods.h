#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Mods (game/mods.cpp): the folders in mods beside the game, each a mod. A mod's files/ holds files by their path in
// the game's paks (files/assets/ts/textures/xbox/<name>.xbt, a model's .xbr...), read in place of the paks' own, or
// added when no pak has them. mod.toml beside it may give name = "...", author = "..." and description = "...". The
// mods are taken in their folders' order and the first that has a file gives it. A mod switched off (the settings
// panel's Mods page, settings file mods_off) gives nothing; a change is seen by the files read from then on, so in the
// next level.
namespace game::mods {

struct Mod {
    std::string folder;  // its folder's name
    std::string name, author, description;
    size_t files = 0;  // in its files/
    bool on = true;
};

// the mods found, indexed again (the folder read anew) when `rescan`
std::vector<Mod> List(bool rescan = false);
void SetOn(const std::string& folder, bool on);
// the file at a path in the paks (as the game names it: "host0:" and the loader's .raw for .xbt allowed) from the
// first mod switched on that has it; nullptr when none. What it points to stays while the game runs.
const std::vector<uint8_t>* File(const std::string& path);

// A mod's first-person model (a .xbr named *_anim) adjusted as it is served: scaled (length along z, girth across),
// turned (degrees) and moved about the middle of its box. Kept in the mod's adjust.toml, a table per model name.
struct Adjust {
    float scale = 1, length = 1, girth = 1;
    float yaw = 0, pitch = 0, roll = 0;
    float x = 0, y = 0, z = 0;
    bool operator==(const Adjust&) const = default;
};
struct Adjustable {
    std::string key;   // the path in the paks
    std::string name;  // the model's, e.g. sbp500_anim
    Adjust adjust;
    bool loaded = false;  // in the game's memory now: a change shows at once
};
// the file at `path` read into the game's memory at `buffer` (ReadFile)
void Read(const std::string& path, uint32_t buffer);
// the mods' first-person models the game has read, the last read first
std::vector<Adjustable> Adjustables();
// the model's adjustment set, into the game's memory when it is there; `save` writes the mod's adjust.toml
void SetAdjust(const std::string& key, const Adjust& a, bool save);

}  // namespace game::mods
