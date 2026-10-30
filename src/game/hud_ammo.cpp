// The early Future Perfect's ammunition, which was TimeSplitters 2's: at the bottom of the view the rounds left, then
// the clip as a strip of rounds (hudpistols, hudclip16, hudsniper...), an icon of the ammunition, a tank or a heat
// gauge, by the gun. The final game kept TimeSplitters 2's drawing of it (0x1334D0, its 0xBF0A0): each ammunition
// type (0x4EA3FC, 0xE0 bytes each: max at -4, icon sprite +0, 4 elements from +8, name +0x88) lists up to four
// elements of 0x20 bytes, {kind, sprite, sprite, 0, 0, scale, width or 0, rounds an image shows*}, laid out left to
// right. Kinds 1 to 7 are TimeSplitters 2's: 1 the rounds left, 2 an icon, 3 a tank (base and colour sprites), 4 the
// clip, 5 the rounds in the clip, 6 a heat gauge (two sprites), 7 a gap; the final game draws its own with kinds 10 to
// 18 (0x1325D0). In the early style the ammunition types get TimeSplitters 2's elements (its 0x2C1678, of the nearest
// TimeSplitters 2 ammunition) while the HUD is drawn.
//
// Their sprites are still in the game's table (142 to 178) but their textures are no longer in its paks: they are read
// from the player's own TimeSplitters 2 (textures/misc/<name>.xbt in its paks; the game reads TimeSplitters 2's
// textures as they are) and served as the game asks for them (briefing.cpp). A gun's textures are loaded with it when
// a level starts (0x1B5C70, by 0x810C0 for its ammunition's sprites); all of TimeSplitters 2's are loaded then too,
// whatever the style and the gun, so it can change while a level is played and a gun given later finds them.
//
// The crosshairs too: each gun has a type of crosshair (gun +0x54) and each type a sprite and a size (0x4E6900, 0x1C
// bytes each: sprite +0xC, half its size +0x10), drawn by 0x14C970. In the early style, as in the 2004 preview, the
// guns of bullets have crosshair_circle (TimeSplitters 2's, no longer in the game's paks), the sci-fi ones
// crosshair_triangle, the shotguns crosshair_shaded and the homing launcher homingcrosshair.
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/mods.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game {
std::map<std::string, std::vector<uint8_t>> Ts2Files(const std::set<std::string>& names);  // game/ts2_style.cpp
std::filesystem::path Ts2Root();                                                          // game/ts2_style.cpp
}

namespace {

constexpr uint32_t kAmmoTypes = 0x4EA3FC, kAmmoTypeSize = 0xE0, kAmmoTypeCount = 0x2C;
constexpr uint32_t kElements = 8, kElementWords = 8, kElementCount = 4;  // in an ammunition type
constexpr uint32_t kGuns = 0x4F9BAC;  // [ ]: 0x29C bytes a gun, its ammunition +0x14 and +0x1C
constexpr uint32_t kGunSize = 0x29C;
constexpr uint32_t kSprites = 0x4D2D58, kSpriteSize = 0x18;  // {group flags, name, 0, texture or -1, ...}
constexpr uint32_t kLoadSprite = 0x810C0;                     // (sprite): its texture loaded with the level's
constexpr uint32_t kSpriteTexture = 0x80F20;                  // (sprite): its texture
constexpr uint32_t kTextures = 0x4B85C8;                      // [[ ]]: 0x34 bytes a texture, width +0x26, height +0x2A
constexpr uint32_t kElementWidth = 0x131710, kElementHeight = 0x1318C0;  // (type, element[, 0])
constexpr uint32_t kSetTexture = 0x1ED50, kQuad = 0x1F090;  // quad (l, t, r, b, u0, v0, u1, v1 in 16ths, colour)
constexpr uint32_t kArrowsInImage = 4;                      // hudarrows
constexpr char kGameFolder[] = "assets/ts/textures/xbox/";    // where the game asks for a sprite's texture
constexpr char kTs2Folder[] = "textures/misc/";               // where TimeSplitters 2's paks have it

// the sprites (their names in TimeSplitters 2 and here alike)
enum Sprite : uint32_t {
    kSmall = 142, kShell, kBig, kSniper, kPetrol, kFoam, kLaser, kPlasmaMachine, kSticky, kPlasma, kRocket, kHoming,
    kProx, kRemote, kTimed, kTnt, kFlare, kBow, kElectro, kGrenade, kBrick,
    kOverheat = 164, kOverheatColour, kClip16, kArrows, kPistols, kShells, kSniperClip, kGrenades, kRockets, kTankBase,
    kTankFlamer, kTankLaser, kTankElectro, kTankPowder, kTankExtinguish,
    kNone = 0xFFFFFFFF
};
constexpr uint32_t kFirstSprite = kSmall, kLastSprite = kTankExtinguish;
constexpr uint32_t kCircle = 274, kTriangle = 275, kShaded = 276, kHomingCrosshair = 286;
constexpr uint32_t kHudBack = 249;  // the rank's ground (hud_style.cpp)
constexpr uint32_t kEarlySprites[] = {kCircle, kTriangle, kShaded, kHomingCrosshair, kHudBack};  // loaded with every level

constexpr uint32_t kCrosshairs = 0x4E6900, kCrosshairSize = 0x1C;  // {.., sprite +0xC, half the size +0x10, ..}
struct Crosshair {
    uint32_t sprite;
    float size;
};
// the early style's crosshairs, by the type (TimeSplitters 2's sizes: circle 15, triangle 14, shaded 20)
const std::map<uint32_t, Crosshair> kEarlyCrosshairs = {
    {1, {kCircle, 15}},            // machine gun, rocket launcher
    {2, {kCircle, 15}},            // pistols, flare gun, harpoon, monkey gun
    {4, {kCircle, 15}},            // sniper rifles
    {7, {kCircle, 15}},            // sci-fi sniper
    {16, {kTriangle, 14}},         // Mag-Charger
    {17, {kTriangle, 14}},         // Mag-Charger's second mode
    {18, {kCircle, 15}},           // revolver
    {21, {kShaded, 20}},           // shotguns
    {22, {kCircle, 15}},           // minigun, K-SMG
    {23, {kCircle, 15}},           // Soviet rifle
    {24, {kCircle, 15}},           // rockets
    {25, {kHomingCrosshair, 22}},  // missiles
    {31, {kHomingCrosshair, 22}},  // homing launcher (with its lock-on)
};

// an element as TimeSplitters 2's: rounds, the rounds an image of the clip shows (0 for none)
struct Element {
    uint32_t kind, a = kNone, b = kNone;
    float scale = 1;
    uint32_t width = 0, rounds = 0;
};
using Elements = std::vector<Element>;
// TimeSplitters 2's ammunition (0x2C1678, 0xD0 bytes each, its elements from +8), by the guns that fire it
const Elements kPistol = {{1}, {7, kNone, kNone, 1, 4}, {4, kPistols, kNone, 0.5f, 0, 8}};         // 1
const Elements kRifle = {{1}, {2, kBig, kNone, 0.8f, 16}, {4, kClip16, kNone, 0.5f, 0, 8}};        // 2, 9
const Elements kMinigun = {{1}, {2, kBig, kNone, 0.8f, 16}, {6, kOverheat, kOverheatColour, 0.7f}};  // 3
const Elements kSniperRifle = {{1}, {7, kNone, kNone, 1, 4}, {4, kSniperClip, kNone, 0.6f, 0, 8}};  // 4
const Elements kShotgun = {{1}, {7, kNone, kNone, 1, 4}, {4, kShells, kNone, 0.7f, 0, 8}};          // 6
const Elements kPlasmaRifle = {{1}, {2, kPlasmaMachine, kNone, 0.7f, 24}, {6, kOverheat, kOverheatColour, 0.7f}};
const Elements kPlasmaPistol = {{1}, {2, kPlasma, kNone, 0.7f, 24}, {5}};                            // 12
const Elements kHomingRockets = {{1}, {2, kHoming, kNone, 0.8f}};                                   // 14
const Elements kBricks = {{1}, {2, kBrick, kNone, 0.8f}};                                           // 17
const Elements kRemoteMines = {{1}, {2, kRemote, kNone, 0.8f}};                                     // 18
const Elements kTimedMines = {{1}, {2, kTimed, kNone, 0.8f}};                                       // 19
const Elements kProxMines = {{1}, {2, kProx, kNone, 0.8f}};                                         // 20
const Elements kTntPack = {{2, kTnt, kNone, 0.8f, 24}};                                             // 21
const Elements kElectroTank = {{2, kElectro, kNone, 0.8f, 32}, {3, kTankBase, kTankElectro, 0.8f}};  // 22
const Elements kPetrolTank = {{2, kPetrol, kNone, 0.7f, 32}, {3, kTankBase, kTankFlamer, 0.8f}};     // 23
const Elements kArrowClip = {{1}, {7, kNone, kNone, 1, 4}, {4, kArrows, kNone, 0.8f, 0, 4}};        // 25
const Elements kStickyBombs = {{1}, {2, kSticky, kNone, 0.8f, 24}};                                 // 27
// the early style's own: the machine gun's and the SBP500's clips (64 rounds) as a number, a rocket, a hand grenade,
// one arrow of hudarrows (drawn by sub_00131B80), a turret's heat
const Elements kRifleCount = {{1}, {2, kBig, kNone, 0.8f, 16}, {5}};
const Elements kRocket1 = {{1}, {2, kRocket, kNone, 0.8f}};
const Elements kHandGrenades = {{1}, {2, kGrenade, kNone, 0.8f}};  // thrown: ammo_grenade, not the launcher's clip
const Elements kHarpoon = {{1}, {2, kArrows, kNone, 0.8f, 13}};
const Elements kTurret = {{6, kOverheat, kOverheatColour, 0.7f}};
const Elements kPlasmaTurret = {{2, kPlasmaMachine, kNone, 0.7f, 24}, {6, kOverheat, kOverheatColour, 0.7f}};
const Elements kDispersion = {{1}, {2, kElectro, kNone, 0.8f, 16}, {6, kOverheat, kOverheatColour, 0.7f}};

// this game's ammunition (by its names) as TimeSplitters 2's nearest
const std::map<uint32_t, const Elements*> kStyles = {
    {1, &kPistol},         // pistol
    {2, &kPistol},         // Kruger
    {3, &kPistol},         // LX-29
    {4, &kMinigun},        // minigun
    {5, &kSniperRifle},    // sniper
    {6, &kSniperRifle},    // vintage rifle
    {7, &kSniperRifle},    // sci-fi sniper
    {8, &kRifle},          // Soviet rifle
    {9, &kShotgun},        // shotgun shells
    {10, &kShotgun},       // 12-gauge shells
    {11, &kDispersion},    // dispersion gun
    {12, &kPistol},        // revolver
    {13, &kRifleCount},    // machine gun
    {14, &kRifleCount},    // SBP500
    {15, &kRifle},         // K-SMG
    {16, &kRocket1},       // K-SMG rockets
    {17, &kPlasmaRifle},   // plasma
    {18, &kPlasmaPistol},  // sci-fi handgun
    {19, &kRocket1},       // rockets
    {20, &kHomingRockets}, // heatseeking rockets
    {21, &kRocket1},       // rockets
    {22, &kHandGrenades},  // grenades
    {23, &kHandGrenades},  // time disrupter grenades
    {24, &kBricks},        // bricks
    {25, &kRemoteMines},   // remote mines
    {26, &kTimedMines},    // timed mines
    {27, &kProxMines},     // proximity mines
    {28, &kTntPack},       // TNT
    {29, &kElectroTank},   // electricity
    {30, &kElectroTank},   // ghost gun
    {31, &kPetrolTank},    // petrol
    {32, &kHarpoon},       // harpoons
    {33, &kArrowClip},     // darts
    {34, &kStickyBombs},   // plasma grenades
    {35, &kPistol},
    {36, &kTurret},        // turrets
    {37, &kRocket1},       // flares
    {40, &kPlasmaTurret},  // plasma turrets, the big gun of Something to Crow About
    {41, &kRifle},         // Mag-Charger
    {43, &kPlasmaRifle},   // monkey gun
};

std::string g_folder;                                    // the TimeSplitters 2 folder the textures were read from
std::map<std::string, std::vector<uint8_t>> g_textures;  // TimeSplitters 2's, by the sprite's name
uint32_t g_rounds = 0;                                   // three ints: 8, 4 and 3 rounds an image

std::string SpriteName(uint32_t sprite) {
    return kernel::ReadCString(kernel::R32(kSprites + sprite * kSpriteSize + 4));
}
bool Loaded(uint32_t sprite) { return kernel::R32(kSprites + sprite * kSpriteSize + 0xC) != kNone; }

// TimeSplitters 2's textures of the sprites, read again when its folder changed
void ReadTextures() {
    std::string folder = game::Ts2Root().string();
    if (folder == g_folder) return;
    g_folder = folder;
    g_textures.clear();
    if (folder.empty()) {
        LOGR("hud: TimeSplitters 2's ammunition needs its folder (General > TimeSplitters 2 folder)");
        return;
    }
    std::set<std::string> names;
    for (uint32_t s = kFirstSprite; s <= kLastSprite; s++) names.insert(kTs2Folder + SpriteName(s) + ".xbt");
    names.insert(kTs2Folder + SpriteName(kCircle) + ".xbt");
    for (auto& [name, bytes] : game::Ts2Files(names))
        g_textures[name.substr(sizeof kTs2Folder - 1, name.size() - (sizeof kTs2Folder - 1) - 4)] = std::move(bytes);
    LOGR("hud: %zu of TimeSplitters 2's %zu HUD sprites read from %s", g_textures.size(), names.size(),
         folder.c_str());
}

// the elements of an ammunition type put in place (TimeSplitters 2's, or the ones saved)
void Write(uint32_t type, const uint32_t* words) {
    for (uint32_t i = 0; i < kElementCount * kElementWords; i++)
        kernel::W32(kAmmoTypes + type * kAmmoTypeSize + kElements + i * 4, words[i]);
}

std::array<uint32_t, kElementCount * kElementWords> Ts2Elements(const Elements& list) {
    std::array<uint32_t, kElementCount * kElementWords> w{};
    for (size_t i = 0; i < list.size() && i < kElementCount; i++) {
        const Element& e = list[i];
        uint32_t* o = &w[i * kElementWords];
        uint32_t rounds = e.rounds == 8 ? g_rounds : e.rounds == 4 ? g_rounds + 4 : e.rounds == 3 ? g_rounds + 8 : 0;
        o[0] = e.kind, o[1] = e.a, o[2] = e.b, o[5] = std::bit_cast<uint32_t>(e.scale), o[6] = e.width, o[7] = rounds;
    }
    return w;
}

// the sprites an ammunition type's elements need, all loaded
bool Ready(const Elements& list, std::string* missing) {
    for (const Element& e : list)
        for (uint32_t s : {e.a, e.b})
            if (s != kNone && !Loaded(s)) {
                if (missing) *missing = SpriteName(s);
                return false;
            }
    return true;
}

}  // namespace

namespace game {

// briefing.cpp: the texture of a game path ("assets/ts/textures/xbox/<sprite>.xbt") when it is one of TimeSplitters
// 2's HUD sprites read
const std::vector<uint8_t>* Ts2Sprite(const std::string& path) {
    if (path.size() <= sizeof kGameFolder - 1 + 4 || path.compare(0, sizeof kGameFolder - 1, kGameFolder)) return nullptr;
    auto it = g_textures.find(path.substr(sizeof kGameFolder - 1, path.size() - (sizeof kGameFolder - 1) - 4));
    if (it == g_textures.end()) return nullptr;
    static std::set<std::string> served;
    if (served.insert(it->first).second)
        LOGR("hud: TimeSplitters 2's %s served (%zu bytes)", it->first.c_str(), it->second.size());
    return &it->second;
}

}  // namespace game

// a gun's things loaded (gun, 0 to load its textures too, ?): TimeSplitters 2's sprites for its ammunition as well
X86_EXTERN_FUNC(__imp__sub_001B5C70);
X86_FUNC(sub_001B5C70) {
    uint32_t gun = kernel::R32(ctx.esp + 4), no_textures = kernel::R32(ctx.esp + 8);
    __imp__sub_001B5C70(ctx, base);
    uint32_t guns = kernel::R32(kGuns);
    if (no_textures || int32_t(gun) < 0 || !guns) return;
    if (!g_rounds && (g_rounds = mem::AllocSystem(12))) {
        kernel::W32(g_rounds, 8);
        kernel::W32(g_rounds + 4, 4);
        kernel::W32(g_rounds + 8, 3);
    }
    ReadTextures();
    uint32_t eax = ctx.eax;
    for (uint32_t s = kFirstSprite; s <= kLastSprite; s++)
        if (g_textures.count(SpriteName(s))) kernel::CallGuest(ctx, kLoadSprite, {s});
    for (uint32_t s : kEarlySprites)  // the circle only when TimeSplitters 2's was read
        if (s != kCircle || g_textures.count(SpriteName(s))) kernel::CallGuest(ctx, kLoadSprite, {s});
    ctx.eax = eax;
}

// an icon of an ammunition (type, element, x, y, second sprite, fade): the harpoon's is one arrow of hudarrows, the
// image's first quarter
X86_EXTERN_FUNC(__imp__sub_00131B80);
X86_FUNC(sub_00131B80) {
    uint32_t type = kernel::R32(ctx.esp + 4), element = kernel::R32(ctx.esp + 8);
    if (kernel::R32(element) != 2 || kernel::R32(element + 4) != kArrows || kernel::R32(ctx.esp + 0x14)) {
        __imp__sub_00131B80(ctx, base);
        return;
    }
    int32_t x = int32_t(kernel::R32(ctx.esp + 0xC)), y = int32_t(kernel::R32(ctx.esp + 0x10));
    float fade = std::bit_cast<float>(kernel::R32(ctx.esp + 0x18));
    uint32_t texture = kernel::CallGuest(ctx, kSpriteTexture, {uint32_t(kArrows)});
    uint32_t record = kernel::R32(kernel::R32(kTextures)) + texture * 0x34;
    uint32_t u = kernel::R16(record + 0x26) / kArrowsInImage, v = kernel::R16(record + 0x2A);
    int32_t w = int32_t(kernel::CallGuest(ctx, kElementWidth, {type, element, 0})) / int32_t(kArrowsInImage) / 2;
    int32_t h = int32_t(kernel::CallGuest(ctx, kElementHeight, {type, element})) / 2;
    uint32_t colour = 0x7F7F7F00 | (uint32_t(int32_t(fade * 127.0f)) & 0xFF);
    kernel::CallGuest(ctx, kSetTexture, {texture});
    kernel::CallGuest(ctx, kQuad, {uint32_t(x - w), uint32_t(y - h), uint32_t(x + w), uint32_t(y + h), 0, 0, u << 4,
                                   v << 4, colour});
    kernel::Return(ctx, 0, 0);
}

// a hand's ammunition at the bottom of the view: TimeSplitters 2's elements in the early style, for the ammunition
// whose sprites are loaded
X86_EXTERN_FUNC(__imp__sub_001334D0);
X86_FUNC(sub_001334D0) {
    if (game::Opt().hud_style != 1 || !g_rounds) {
        __imp__sub_001334D0(ctx, base);
        return;
    }
    static std::map<uint32_t, bool> logged;  // by ammunition type: whether TimeSplitters 2's was drawn
    std::vector<std::pair<uint32_t, std::array<uint32_t, kElementCount * kElementWords>>> saved;
    for (auto& [type, list] : kStyles) {
        std::string missing;
        bool ready = Ready(*list, &missing);
        if (ready) {
            std::array<uint32_t, kElementCount * kElementWords> was;
            for (uint32_t i = 0; i < was.size(); i++)
                was[i] = kernel::R32(kAmmoTypes + type * kAmmoTypeSize + kElements + i * 4);
            saved.emplace_back(type, was);
            Write(type, Ts2Elements(*list).data());
        }
        // once a type is in use (its icon loaded), whether it is drawn as TimeSplitters 2's
        uint32_t icon = kernel::R32(kAmmoTypes + type * kAmmoTypeSize);
        if (icon < 0x3FF && Loaded(icon) && (!logged.count(type) || logged[type] != ready)) {
            logged[type] = ready;
            if (ready) LOGR("hud: ammunition %u drawn as TimeSplitters 2's", type);
            else LOGR("hud: ammunition %u drawn as the game's: %s not loaded", type, missing.c_str());
        }
    }
    __imp__sub_001334D0(ctx, base);
    for (auto& [type, was] : saved) Write(type, was.data());
}

// the crosshair of the view's player (0x14C970, with the view; 0x14D6E0 in a vehicle or at a gun emplacement): the
// early style's sprite and size in its type while it is drawn
void DrawEarlyCrosshair(X86Context& ctx, uint8_t* base, void (*Draw)(X86Context&, uint8_t*)) {
    if (game::Opt().hud_style != 1) {
        Draw(ctx, base);
        return;
    }
    std::vector<std::array<uint32_t, 3>> saved;  // type, sprite, size
    for (auto& [type, early] : kEarlyCrosshairs) {
        uint32_t at = kCrosshairs + type * kCrosshairSize;
        if (!Loaded(early.sprite)) continue;
        saved.push_back({type, kernel::R32(at + 0xC), kernel::R32(at + 0x10)});
        kernel::W32(at + 0xC, early.sprite);
        kernel::W32(at + 0x10, std::bit_cast<uint32_t>(early.size));
    }
    // once a type is drawn, whether it was the early style's
    uint32_t player = kernel::R32(0x7B0C7C), type = player ? kernel::R32(0x7A6160 + kernel::R32(player + 4) * 0x177C) : 0;
    static std::map<uint32_t, bool> logged;
    auto early = kEarlyCrosshairs.find(type);
    if (early != kEarlyCrosshairs.end()) {
        bool loaded = Loaded(early->second.sprite);
        if (!logged.count(type) || logged[type] != loaded) {
            logged[type] = loaded;
            LOGR("hud: crosshair %u drawn as %s%s", type, SpriteName(early->second.sprite).c_str(),
                 loaded ? "" : " (not loaded: the game's)");
        }
    }
    Draw(ctx, base);
    for (auto& [type, sprite, size] : saved) {
        kernel::W32(kCrosshairs + type * kCrosshairSize + 0xC, sprite);
        kernel::W32(kCrosshairs + type * kCrosshairSize + 0x10, size);
    }
}

X86_EXTERN_FUNC(__imp__sub_0014C970);
X86_FUNC(sub_0014C970) { DrawEarlyCrosshair(ctx, base, __imp__sub_0014C970); }

X86_EXTERN_FUNC(__imp__sub_0014D6E0);
X86_FUNC(sub_0014D6E0) { DrawEarlyCrosshair(ctx, base, __imp__sub_0014D6E0); }

// The weapon bar (0x141E30, the view's player's): each group of guns shows the icon of the first gun of it the player
// has (0x141BF0; icons at 0x4F9BB0, 0x14 bytes a gun: sprite, its "out" sprite). The Temporal Uplink is two guns, 56
// and 57 with the map, so once the map is had its icon showed in the bar whatever was held. With the early Uplink's
// icons (mod Early HUD extras: the map one green), the map's is shown only while the Uplink with the map is in hand
// (the character's gun +0x94); otherwise the Uplink's own
namespace {
constexpr uint32_t kGunIcons = 0x4F9BB0, kGunIconSize = 0x14, kUplink = 56, kUplinkMap = 57;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00141E30);
X86_FUNC(sub_00141E30) {
    uint32_t player = kernel::R32(0x7B0C7C), body = player ? kernel::R32(player + 0x14) : 0;
    uint32_t chr = body ? kernel::R32(body + 0x7C) : 0;
    uint32_t map = kGunIcons + kUplinkMap * kGunIconSize, own = kGunIcons + kUplink * kGunIconSize;
    bool swap = chr && kernel::R32(chr + 0x94) != kUplinkMap &&
                game::mods::File("assets/ts/textures/xbox/gunim_uplink_map_b.xbt");
    uint32_t saved[2] = {kernel::R32(map), kernel::R32(map + 4)};
    if (swap) kernel::W32(map, kernel::R32(own)), kernel::W32(map + 4, kernel::R32(own + 4));
    __imp__sub_00141E30(ctx, base);
    if (swap) kernel::W32(map, saved[0]), kernel::W32(map + 4, saved[1]);
}
