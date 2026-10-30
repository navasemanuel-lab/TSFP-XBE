// Two cheats the game cut, back in Extras > Cheats.
//
// The cheats: 13 entries of 0x1C at 0x4FCC80 (+0 name text, +4 the kinds of game allowed in, 4 Story and 2 Arcade,
// +8 what unlocks it, 0 nothing). Available(i) (sub_001CD940) is false for an entry nothing unlocks: Infinite Ammo
// (4, text 0x16B6), Skating (7) and Silly Hats (12) are never offered. The chosen ones are bits of [0x790954]; a level
// takes those its kind allows into [0x790958] (sub_001CD170: Story or Arcade, none in a network game or a challenge)
// and a cheat is on with its bit in both and Available. Infinite Ammo is still read (bit 0x10, 0x1BA355): it is made
// available here.
//
// Invincible (text 0x16B2) has no entry; the game keeps a debug flag of its own ([0x6D612C], the command line's
// "invincible") for every player. Here it is bit 13 of [0x790954], a row of the Cheats menu of its own:
// - the menu (built by 0x2CC420) gets the row last, before its rows are laid out (the FinishMenu hook, quit_menu.cpp);
// - the rows are read into the bits by 0x2CC390 and set from them by 0x2CC2C0, rows 0-12: row 13 alongside;
// - on in a level whose kind takes cheats ([0x790958] some), the local players' bodies are unhurt: their damage
//   (sub_000BABA0, free_cam.cpp) dropped and their health kept full.
#include <bit>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kChosen = 0x790954, kAllowed = 0x790958;
constexpr uint32_t kInfiniteAmmo = 4, kInvincible = 13, kInvincibleText = 0x16B2;
constexpr uint32_t kFromCheatsMenu = 0x2CC579;  // its rows laid out
constexpr uint32_t kText = 0x3D7B0, kAddRow = 0x79D60, kRowChoices = 0x7D480, kRowDone = 0x71830;
constexpr uint32_t kRowOf = 0x71940;  // (menu, id): the row
constexpr uint32_t kRowChoice = 0xC8;
constexpr uint32_t kPlayers = 0x790950, kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584, kBody = 0x14;
constexpr uint32_t kHealth = 0x1CC, kMostHealth = 0x1D8;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
bool Chosen(uint32_t bit) { return (kernel::R32(kChosen) >> bit) & 1; }
uint32_t Row(X86Context& ctx, uint32_t menu, uint32_t id) {
    uint32_t row = kernel::CallGuest(ctx, kRowOf, {menu, id});
    return Valid(row, kRowChoice + 4) ? row : 0;
}
bool InvincibleNow() { return Chosen(kInvincible) && kernel::R32(kAllowed) != 0; }
}  // namespace

namespace game {
// the FinishMenu hook: the Invincible row after the Cheats menu's own
void AddCheatRow(X86Context& ctx) {
    if (kernel::R32(ctx.esp) != kFromCheatsMenu) return;
    X86Context saved = ctx;
    uint32_t text = kernel::CallGuest(ctx, kText, {kInvincibleText});
    kernel::CallGuest(ctx, kAddRow, {text, kInvincible, 0, 0, 0, 0});
    kernel::CallGuest(ctx, kRowChoices, {0, 0});
    kernel::CallGuest(ctx, kRowDone, {});
    ctx = saved;
}

// the damage hook (free_cam.cpp): a local player's body, Invincible on
bool CheatUnhurt(uint32_t body) {
    if (!body || !InvincibleNow()) return false;
    uint32_t players = kernel::R32(kPlayerArray), count = kernel::R32(kPlayers);
    for (uint32_t i = 0; i < count && i < 4; i++)
        if (kernel::R32(players + i * kPlayerSize + kBody) == body) return true;
    return false;
}

// each frame (free_cam.cpp's view hook): the local players' health full, Invincible on
void CheatKeepUnhurt() {
    if (!InvincibleNow()) return;
    uint32_t players = kernel::R32(kPlayerArray), count = kernel::R32(kPlayers);
    if (!Valid(players, kPlayerSize)) return;
    for (uint32_t i = 0; i < count && i < 4; i++) {
        uint32_t player = players + i * kPlayerSize;
        uint32_t body = Valid(player, kBody + 4) ? kernel::R32(player + kBody) : 0;
        if (!Valid(body, kMostHealth + 4)) continue;
        // not one already dead (what kills outright, as a fall out of the level, is the game's)
        float health = std::bit_cast<float>(kernel::R32(body + kHealth));
        if (health > 0.0f && kernel::R32(body + kHealth) != kernel::R32(body + kMostHealth))
            kernel::W32(body + kHealth, kernel::R32(body + kMostHealth));
    }
}
}  // namespace game

// Available(cheat): Infinite Ammo, which nothing unlocks, offered
X86_EXTERN_FUNC(__imp__sub_001CD940);
X86_FUNC(sub_001CD940) {
    if (kernel::R32(ctx.esp + 4) == kInfiniteAmmo) return kernel::Return(ctx, 1, 0);
    __imp__sub_001CD940(ctx, base);
}

// the Cheats menu's rows into the chosen bits: Invincible's too (before, as this ends setting the rows again)
X86_EXTERN_FUNC(__imp__sub_002CC390);
X86_FUNC(sub_002CC390) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    {
        X86Context saved = ctx;
        if (uint32_t row = Row(ctx, menu, kInvincible)) {
            uint32_t chosen = kernel::R32(kChosen) & ~(1u << kInvincible);
            if (kernel::R32(row + kRowChoice) == 1) chosen |= 1u << kInvincible;
            if (chosen != kernel::R32(kChosen)) LOGR("cheats: Invincible %s", (chosen >> kInvincible) & 1 ? "on" : "off");
            kernel::W32(kChosen, chosen);
        }
        ctx = saved;
    }
    __imp__sub_002CC390(ctx, base);
}

// the chosen bits into the Cheats menu's rows: Invincible's too
X86_EXTERN_FUNC(__imp__sub_002CC2C0);
X86_FUNC(sub_002CC2C0) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    __imp__sub_002CC2C0(ctx, base);
    X86Context saved = ctx;
    if (uint32_t row = Row(ctx, menu, kInvincible)) kernel::W32(row + kRowChoice, Chosen(kInvincible));
    ctx = saved;
}
