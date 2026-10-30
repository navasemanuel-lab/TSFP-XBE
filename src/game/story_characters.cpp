// Story's players' characters. Each Story mission's row (0x4D1AC0, 13 of 0x44: +6 the scene) names its two players'
// character types, +0xC player 1 (Cortez) and +0xE player 2 (the partner), read when the mission loads its
// characters and voices (0x64DD0, 0x65940, both (player, scene)). Options::story_character puts any Arcade
// character in either place: the rows are written before those reads, and put back when the option is 0 again.
//
// A Story level has only its own characters: its resource table is the generic one and the level's
// (prop/table/prop_generic_xbas.raw, 0x242780 through 0x62F50), not Arcade's whole one (prop_all_xbas.raw), and its
// pak only its own models. Another character's resource then has no entry, and its load (0xA5E40 -> 0x63A40) read a
// name from nowhere. With a character chosen, Story is given the whole table in place of the generic one (from the
// front end's pak, which Arcade's levels all carry) and the models and textures its pak lacks from chr.pak.
//
// A character's body is sized by its model (0xD2FD0, from the model's size 0x8E5F0, 1 when it has none): the type's
// floats +0x1C (height, 1.8 a size, at most 2.2), +0x20 (the size), +0x24 (half a size) and +0x28 (the size), the
// last two 0.5 and 1 when it is no bigger than 1. A big character then fills doors and passages Cortez walks through;
// a chosen one is given Cortez's body.
//
// The Arcade list (0x521A18, 150 of 0x30: +4 the character type) gives the characters; a character type's name is its
// text (0x53A3C8, 0x30 each, +0; 0x2BE3C8), in the common texts.
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace game {
std::string GameText(uint32_t id);                                          // game/discord.cpp
void MansionSetupLoaded(X86Context& ctx, uint32_t name, uint32_t size_at);  // game/mansion_cut.cpp
}

namespace {
constexpr uint32_t kRows = 0x4D1AC0, kRowSize = 0x44, kRowCount = 13, kPlayerType[2] = {0xC, 0xE};
constexpr uint32_t kArcade = 0x521A18, kArcadeSize = 0x30, kArcadeCount = 150, kArcadeType = 4;
constexpr uint32_t kTypes = 0x53A3C8, kTypeSize = 0x30;  // +0 the name text
constexpr uint32_t kBody = 0x1C, kCortez = 12;           // the body's 4 floats; Cortez's type
constexpr uint32_t kGameMode = 0x7DE454;                 // its second byte 10 in Story
// the names 0x242780 loads the resource table by
constexpr uint32_t kGenericTable = 0x49EFF4, kWholeTable = 0x49F01C;
constexpr char kFrontEndPak[] = "D:\\pak\\arcade\\l_101.pak", kChrPak[] = "D:\\pak\\chr.pak";

uint16_t g_own[kRowCount][2];  // each row's own types
bool g_kept = false;
std::vector<std::string> g_names;

bool Chosen() {
    for (uint32_t pick : game::Opt().story_character)
        if (pick >= 1 && pick <= kArcadeCount) return true;
    return false;
}

bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }

// the pak's names are the CRC-32 of the path
uint32_t Crc32(const std::string& s) {
    uint32_t c = ~0u;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// a pak read whole once: {"P5CK", directory offset, directory size}, the directory 16 bytes a file {hash, offset,
// size, 0}; each file asked for copied out once
struct Pak {
    const char* path;
    bool read = false;
    std::vector<uint8_t> data;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> dir;
    std::map<uint32_t, std::vector<uint8_t>> files;

    const std::vector<uint8_t>* File(uint32_t hash) {
        if (!read) {
            read = true;
            if (!kernel::ReadGameFile(path, data) || data.size() < 16 || std::memcmp(data.data(), "P5CK", 4)) {
                LOGR("story: no %s", path);
                return nullptr;
            }
            uint32_t at, size;
            std::memcpy(&at, &data[4], 4);
            std::memcpy(&size, &data[8], 4);
            for (uint64_t e = at; e + 16 <= uint64_t(at) + size && e + 16 <= data.size(); e += 16) {
                uint32_t h, off, n;
                std::memcpy(&h, &data[e], 4);
                std::memcpy(&off, &data[e + 4], 4);
                std::memcpy(&n, &data[e + 8], 4);
                if (uint64_t(off) + n <= data.size()) dir[h] = {off, n};
            }
        }
        auto it = dir.find(hash);
        if (it == dir.end()) return nullptr;
        auto& f = files[hash];
        if (f.empty()) f.assign(data.begin() + it->second.first, data.begin() + it->second.first + it->second.second);
        return &f;
    }
};
Pak g_front_end{kFrontEndPak}, g_chr{kChrPak};
std::mutex g_lock;

void Apply() {
    if (!g_kept) {
        for (uint32_t r = 0; r < kRowCount; r++)
            for (int p = 0; p < 2; p++) g_own[r][p] = kernel::R16(kRows + r * kRowSize + kPlayerType[p]);
        g_kept = true;
    }
    for (int p = 0; p < 2; p++) {
        uint32_t pick = game::Opt().story_character[p];
        bool arcade = pick >= 1 && pick <= kArcadeCount;
        uint16_t type = arcade ? uint16_t(kernel::R32(kArcade + (pick - 1) * kArcadeSize + kArcadeType)) : 0;
        for (uint32_t r = 0; r < kRowCount; r++) {
            uint32_t at = kRows + r * kRowSize + kPlayerType[p];
            uint16_t want = arcade ? type : g_own[r][p];
            if (kernel::R16(at) == want) continue;
            *static_cast<uint16_t*>(kernel::HostPtr(at)) = want;
            if (r == 0) LOGR("story: player %d is character type %u", p + 1, want);
        }
    }
}
}  // namespace

namespace game {

// "Each mission's own", then the Arcade characters by name (or number, until the game's texts are loaded)
std::vector<std::string> StoryCharacterNames() {
    if (g_names.empty() || g_names[1][0] == '#') {
        std::vector<std::string> names{"Each mission's own"};
        for (uint32_t i = 0; i < kArcadeCount; i++) {
            uint32_t type = kernel::R32(kArcade + i * kArcadeSize + kArcadeType);
            std::string n = GameText(uint32_t(int16_t(kernel::R16(kTypes + type * kTypeSize))));
            names.push_back(n.empty() ? "#" + std::to_string(i + 1) : n);
        }
        g_names = std::move(names);
    }
    return g_names;
}

// a file a Story level lacks with a character chosen (path as the game asks for it): the whole resource table, or a
// model or texture of chr.pak
const std::vector<uint8_t>* StoryCharacterFile(std::string path) {
    if (!Story() || !Chosen()) return nullptr;
    if (size_t at = path.find("host0:"); at != std::string::npos) path = path.substr(at + 6);
    std::lock_guard lock(g_lock);
    if (path == "prop/table/prop_all_xbas.raw") return g_front_end.File(Crc32(path));
    if (path.rfind("assets/ts/models/", 0) && path.rfind("textures/", 0)) return nullptr;
    auto file = g_chr.File(Crc32(path));
    if (!file && path.size() >= 4 && !path.compare(path.size() - 4, 4, ".raw"))
        file = g_chr.File(Crc32(path.substr(0, path.size() - 4) + ".xbt"));
    static int logged = 0;
    if (file && logged++ < 64) LOGR("story: %s from chr.pak", path.c_str());
    return file;
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_00064DD0);
X86_FUNC(sub_00064DD0) {
    Apply();
    __imp__sub_00064DD0(ctx, base);
}

// SetUpCharacters(): the players' characters made, their types' bodies sized; a chosen one's then Cortez's (his own
// when he was sized, else a size 1's)
X86_EXTERN_FUNC(__imp__sub_000D2FD0);
X86_FUNC(sub_000D2FD0) {
    __imp__sub_000D2FD0(ctx, base);
    if (!Story() || !Chosen()) return;
    float body[4] = {1.8f, 1.0f, 0.5f, 1.0f};
    if (kernel::R32(kTypes + kCortez * kTypeSize + kBody + 4))
        for (int i = 0; i < 4; i++)
            body[i] = std::bit_cast<float>(kernel::R32(kTypes + kCortez * kTypeSize + kBody + i * 4));
    for (int p = 0; p < 2; p++) {
        uint32_t pick = game::Opt().story_character[p];
        if (pick < 1 || pick > kArcadeCount) continue;
        uint32_t type = kernel::R32(kArcade + (pick - 1) * kArcadeSize + kArcadeType), at = kTypes + type * kTypeSize + kBody;
        float had[4];
        for (int i = 0; i < 4; i++) {
            had[i] = std::bit_cast<float>(kernel::R32(at + i * 4));
            kernel::W32(at + i * 4, std::bit_cast<uint32_t>(body[i]));
        }
        LOGR("story: player %d's character %u given Cortez's body %.2f %.2f %.2f %.2f (its own %.2f %.2f %.2f %.2f)", p + 1,
             type, body[0], body[1], body[2], body[3], had[0], had[1], had[2], had[3]);
    }
}

// LoadFile(name, ..., &size): Story's generic resource table, the whole one in its place with a character chosen;
// Mansion of Madness's setup with its cut scene back (game/mansion_cut.cpp)
X86_EXTERN_FUNC(__imp__sub_00062F50);
X86_FUNC(sub_00062F50) {
    if (kernel::R32(ctx.esp + 4) == kGenericTable && Story() && Chosen()) {
        kernel::W32(ctx.esp + 4, kWholeTable);
        LOGR("story: the whole resource table, for the chosen characters");
    }
    uint32_t name = kernel::R32(ctx.esp + 4), size_at = kernel::R32(ctx.esp + 12);
    __imp__sub_00062F50(ctx, base);
    game::MansionSetupLoaded(ctx, name, size_at);
}

X86_EXTERN_FUNC(__imp__sub_00065940);
X86_FUNC(sub_00065940) {
    Apply();
    __imp__sub_00065940(ctx, base);
}
