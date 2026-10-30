// A BRIEFING tab on Story's pause screen, beside OPTIONS and STATUS: the level's briefing as the level select shows it,
// its year, name, picture and text. The pause tabs (0x5247C4, twelve of {page, shown(), label}) keep a BRIEFING slot
// the game never shows (its test returns 0, its page draws statistics); the slot gets a test of its own and a page
// built as the level select's briefing (0x2CAC30): a row with the year, drawn by its callback (0x2CA9F0) with the name
// and picture, then the text, under the level select's title. The level pictures are the front end's sprites (group
// 0x1000), not loaded in a level: the level's own is loaded with it, its texture read from the front end's pak
// (arcade/l_101.pak), which a level has not mounted. With three tabs their labels are drawn smaller.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/mods.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // cpu/dispatch.cpp: a guest address that calls host code
}
namespace game {
void WithTextScale(X86Context& ctx, const std::function<void()>& f, float scale);  // game/menu_layout.cpp
const std::vector<uint8_t>* Ts2Sprite(const std::string& path);                   // game/hud_ammo.cpp
const std::vector<uint8_t>* Ts2Replacement(const std::string& path);              // game/hud_ammo.cpp
const std::vector<uint8_t>* BetaCortezFile(const std::string& path);              // game/beta_cortez.cpp
const std::vector<uint8_t>* BetaSkylineFile(const std::string& path);             // game/beta_skyline.cpp
const std::vector<uint8_t>* StoryCharacterFile(std::string path);                 // game/story_characters.cpp
}

namespace {

constexpr uint32_t kBriefingTab = 0x5247C4 + 6 * 12;  // {page, shown(), label 0x17B3 "STATISTICS"}
constexpr uint32_t kBriefingTitle = 0x3DB;             // "BRIEFING", the level select's title
constexpr uint32_t kGameMode = 0x7DE454;               // byte 1: 10 in Story
constexpr uint32_t kMap = 0x79094C;                    // the level being played
// 13 of 0x44 bytes: +0 year text id, +6 map, +0x10 briefing text id, +0x14 picture sprite
constexpr uint32_t kStoryLevels = 0x4D1AC0;
constexpr uint32_t kStoryLevelCount = 13, kStoryLevelSize = 0x44;
constexpr uint32_t kSprites = 0x4D2D58, kSpriteSize = 24;  // {group flags, name, 0, sheet index, ...}
constexpr uint32_t kMenuOfBuild = 0x7BB1E8;                // the menu a page builder fills

// the game's calls
constexpr uint32_t kText = 0x3D7B0;          // (id): the text
constexpr uint32_t kMenuTitle = 0x75B70;     // (flags, title, width percent, 0, back callback)
constexpr uint32_t kAddRow = 0x79D60;        // (text, id, flags, height, select callback, draw callback)
constexpr uint32_t kRowStyle = 0x71630;      // (): the style rows are added with
constexpr uint32_t kStyleApply = 0x79710;    // (1)
constexpr uint32_t kMenuEndRows = 0x75E70;
constexpr uint32_t kFinishMenu = 0x771D0;    // (menu)
constexpr uint32_t kBriefingHeader = 0x2CA9F0;  // the level select's: year, name and picture of the row's level
constexpr uint32_t kPageTick = 0x2F2F50, kPageDraw = 0x2F2FA0;  // a pause page's own, as the game's pages set them
constexpr uint32_t kSpriteWanted = 0x81238;     // the sprite loader's call of 0x80F40 (group flags), ESI the sprite
constexpr float kTabScale = 0.8f;
constexpr float kBoxWidth = 64.0f;  // percent of the screen: the level select's 80 crowded the pause gauges
constexpr char kFrontEndPak[] = "D:\\pak\\arcade\\l_101.pak";
constexpr char kPictureFile[] = "assets/ts/textures/xbox/%s.xbt";  // the texture of a sprite named %s

uint32_t U(float f) { return std::bit_cast<uint32_t>(f); }
bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }

// the current Story level's place in the table, -1 outside Story
int32_t Level() {
    if (!Story()) return -1;
    int32_t map = int32_t(kernel::R32(kMap));
    for (uint32_t i = 0; i < kStoryLevelCount; i++) {
        uint32_t e = kStoryLevels + i * kStoryLevelSize;
        if (int16_t(kernel::R32(e + 6) & 0xFFFF) == map && kernel::R32(e + 0x10)) return int32_t(i);
    }
    return -1;
}

// shown(): Story, with a briefing for the level
void Shown(X86Context& ctx, uint8_t*) {
    int32_t level = Level();
    static uint32_t logged_map = ~0u;
    if (kernel::R32(kMap) != logged_map) {
        logged_map = kernel::R32(kMap);
        LOGR("briefing: mode %u map %d, level %d", (kernel::R32(kGameMode) >> 8) & 0xFF, int32_t(logged_map), level);
    }
    kernel::Return(ctx, level >= 0 ? 1 : 0, 0);
}

// builder (menu): the level select's briefing in the pause page's frame
void Build(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    int32_t level = Level();
    kernel::W32(menu + 0x1C4, kPageTick);
    kernel::W32(menu + 0x1C8, kPageDraw);
    kernel::CallGuest(ctx, kMenuTitle, {0xC, 0, U(kBoxWidth), 0, 0});
    if (level >= 0) {
        uint32_t e = kStoryLevels + uint32_t(level) * kStoryLevelSize;
        uint32_t year = uint32_t(int32_t(int16_t(kernel::R32(e) & 0xFFFF)));
        kernel::CallGuest(ctx, kAddRow, {kernel::CallGuest(ctx, kText, {year}), uint32_t(level), 4, U(19.0f), 0,
                                         kBriefingHeader});
        kernel::W32(kernel::CallGuest(ctx, kRowStyle, {}) + 0x1C, 0xB);
        kernel::W32(kernel::CallGuest(ctx, kRowStyle, {}) + 0x40, U(3.0f));
        kernel::CallGuest(ctx, kStyleApply, {1});
        kernel::CallGuest(ctx, kAddRow, {kernel::CallGuest(ctx, kText, {kernel::R32(e + 0x10)}), 0xFFFFFFFF, 0x20D04,
                                         U(3.0f), 0, 0});
        kernel::CallGuest(ctx, kMenuEndRows, {});
    }
    kernel::CallGuest(ctx, kFinishMenu, {kernel::R32(kMenuOfBuild)});
    kernel::Return(ctx, 0, 0);
}

uint32_t g_page = 0;

// the pak's names are the CRC-32 of the path, after any "host0:" (0x1504F0)
uint32_t Crc32(const char* s) {
    uint32_t c = ~0u;
    for (; *s; s++) {
        c ^= uint8_t(*s);
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// the Story levels' pictures' textures by their name's hash, read from the front end's pak once
const std::map<uint32_t, std::vector<uint8_t>>& Pictures() {
    static std::map<uint32_t, std::vector<uint8_t>> pictures;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<uint8_t> pak;
        if (!kernel::ReadGameFile(kFrontEndPak, pak) || pak.size() < 16 || std::memcmp(pak.data(), "P5CK", 4)) {
            LOGR("briefing: no %s", kFrontEndPak);
            return;
        }
        // {"P5CK", directory offset, directory size}, the directory 16 bytes a file: {hash, offset, size, 0}
        uint32_t dir, dir_size;
        std::memcpy(&dir, &pak[4], 4);
        std::memcpy(&dir_size, &pak[8], 4);
        if (uint64_t(dir) + dir_size > pak.size()) return;
        for (uint32_t i = 0; i < kStoryLevelCount; i++) {
            int32_t picture = int32_t(kernel::R32(kStoryLevels + i * kStoryLevelSize + 0x14));
            if (picture < 0) continue;
            char path[128];
            std::snprintf(path, sizeof(path), kPictureFile,
                          kernel::ReadCString(kernel::R32(kSprites + uint32_t(picture) * kSpriteSize + 4)).c_str());
            uint32_t hash = Crc32(path);
            for (uint32_t e = dir; e + 16 <= dir + dir_size; e += 16) {
                uint32_t h, off, size;
                std::memcpy(&h, &pak[e], 4);
                std::memcpy(&off, &pak[e + 4], 4);
                std::memcpy(&size, &pak[e + 8], 4);
                if (h != hash || uint64_t(off) + size > pak.size()) continue;
                pictures[hash].assign(pak.begin() + off, pak.begin() + off + size);
                break;
            }
        }
        LOGR("briefing: %zu level pictures from the front end", pictures.size());
    });
    return pictures;
}

// the pak path of a file the game asks for (name)
std::string FilePath(uint32_t name) {
    std::string n = kernel::ReadCString(name);
    if (size_t at = n.find("host0:"); at != std::string::npos) n = n.substr(at + 6);
    if (n.size() >= 4 && !n.compare(n.size() - 4, 4, ".raw")) n.replace(n.size() - 4, 4, ".xbt");  // as the loader asks
    return n;
}

// the pak path of a texture the game asks for (name), empty for other files
std::string TexturePath(uint32_t name) {
    std::string n = FilePath(name);
    return n.size() < 4 || n.compare(n.size() - 4, 4, ".xbt") ? std::string() : n;
}

// a level picture's texture, when the file the game asks for (name) is one and in a Story level
const std::vector<uint8_t>* Picture(uint32_t name) {
    if (Level() < 0) return nullptr;
    std::string n = TexturePath(name);
    if (n.empty()) return nullptr;
    auto& pictures = Pictures();
    auto it = pictures.find(Crc32(n.c_str()));
    return it == pictures.end() ? nullptr : &it->second;
}

// a file the paks lack and that is had elsewhere: a level picture, TimeSplitters 2's HUD sprites, the beta Cortez,
// the beta skyline, another character's in Story
const std::vector<uint8_t>* Served(uint32_t name) {
    if (auto p = Picture(name)) return p;
    if (auto p = game::StoryCharacterFile(kernel::ReadCString(name, 256))) return p;
    if (auto p = game::Ts2Sprite(TexturePath(name))) return p;
    if (auto p = game::BetaSkylineFile(FilePath(name))) return p;
    return game::BetaCortezFile(FilePath(name));
}

}  // namespace

// FileSize(name): a file's size in the mounted paks; a mod's first, a texture served when they lack it
X86_EXTERN_FUNC(__imp__sub_00062CA0);
X86_FUNC(sub_00062CA0) {
    uint32_t name = kernel::R32(ctx.esp + 4);
    if (auto p = game::mods::File(kernel::ReadCString(name, 256))) {
        kernel::Return(ctx, uint32_t(p->size()), 0);
        return;
    }
    if (auto p = game::Ts2Replacement(TexturePath(name))) {
        kernel::Return(ctx, uint32_t(p->size()), 0);
        return;
    }
    __imp__sub_00062CA0(ctx, base);
    if (int32_t(ctx.eax) > 0) return;
    if (auto p = Served(name)) ctx.eax = uint32_t(p->size());
}

// ReadFile(name, buffer, most): a file read from the mounted paks, its size; a mod's first, a texture served when they
// lack it
X86_EXTERN_FUNC(__imp__sub_00062D20);
X86_FUNC(sub_00062D20) {
    uint32_t name = kernel::R32(ctx.esp + 4), buffer = kernel::R32(ctx.esp + 8), most = kernel::R32(ctx.esp + 12);
    if (auto p = game::mods::File(kernel::ReadCString(name, 256))) {
        uint32_t n = std::min<uint32_t>(uint32_t(p->size()), most);
        std::memcpy(kernel::HostPtr(buffer), p->data(), n);
        game::mods::Read(kernel::ReadCString(name, 256), buffer);
        kernel::Return(ctx, n, 0);
        return;
    }
    if (auto p = game::Ts2Replacement(TexturePath(name))) {
        uint32_t n = std::min<uint32_t>(uint32_t(p->size()), most);
        std::memcpy(kernel::HostPtr(buffer), p->data(), n);
        kernel::Return(ctx, n, 0);
        return;
    }
    __imp__sub_00062D20(ctx, base);
    if (int32_t(ctx.eax) > 0) return;
    if (auto p = Served(name)) {
        uint32_t n = std::min<uint32_t>(uint32_t(p->size()), most);
        std::memcpy(kernel::HostPtr(buffer), p->data(), n);
        ctx.eax = n;
    }
}

// SpriteWanted(group flags): whether the loader loads a sprite; in a Story level its briefing picture too
X86_EXTERN_FUNC(__imp__sub_00080F40);
X86_FUNC(sub_00080F40) {
    if (kernel::R32(ctx.esp) == kSpriteWanted) {
        int32_t level = Level();
        if (level >= 0) {
            int32_t picture = int32_t(kernel::R32(kStoryLevels + uint32_t(level) * kStoryLevelSize + 0x14));
            if (picture >= 0 && ctx.esi == kSprites + uint32_t(picture) * kSpriteSize) {
                kernel::Return(ctx, 1, 0);
                return;
            }
        }
    }
    __imp__sub_00080F40(ctx, base);
}

// the pause screen's tab labels: smaller with BRIEFING among them
X86_EXTERN_FUNC(__imp__sub_002E4FF0);
X86_FUNC(sub_002E4FF0) {
    if (g_page && Level() >= 0) game::WithTextScale(ctx, [&] { __imp__sub_002E4FF0(ctx, base); }, kTabScale);
    else __imp__sub_002E4FF0(ctx, base);
}

namespace game {

// once the image is in memory: the BRIEFING slot takes the new page and test
void InstallBriefingTab() {
    g_page = mem::AllocSystem(16);
    if (!g_page) return;
    std::memset(kernel::HostPtr(g_page), 0, 16);
    kernel::W32(g_page, dispatch::HostThunk(Build));
    kernel::W32(kBriefingTab, g_page);
    kernel::W32(kBriefingTab + 4, dispatch::HostThunk(Shown));
    kernel::W32(kBriefingTab + 8, kBriefingTitle);
    LOGI("briefing: tab installed");
}

}  // namespace game
