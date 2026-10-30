// The weapon keys (1 to 9 and 0 by default, F1 > Controls): each takes the weapon of its place in the inventory, after
// the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc, pc_weapon_slots_guest.inc).
//
// The player's weapon input is sub_001BF140, for the player being updated ([0x7B0C7C]); after it, the key pressed
// (common/controls.cpp) picks a weapon as the D-pad's cycling would: the weapons in the game's order (0x4FC888, 70
// of them, variants included), each with its group (+0 of its 0x3C entry at 0x4FA160) counted once, kept as the
// cycling keeps them: sub_001BAA70(entity, weapon) the group's preferred variant, sub_001BA410(entity, weapon, 1) held
// and usable. The place's weapon is then taken with sub_001BA130(entity, weapon) and the weapon HUD redrawn
// (sub_00142820). A place with nothing in it does nothing; nothing is ever given.
//
// Only the first input slot's player, the keyboard's. Not in an online co-op session (net/lockstep.h): both machines
// play the same game from the pads alone, and a weapon taken on one only would part them.
#include "common/controls.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kPlayer = 0x7B0C7C, kPlayerSize = 0x1584;  // the player being updated
constexpr uint32_t kSlot = 0x4;                                // the local input slot
constexpr uint32_t kFlags = 0xC, kState = 0x114, kBody = 0x14;
constexpr uint32_t kPauseState = 0x761C48;  // the pause screen's state: +0 bit 0 up
constexpr uint32_t kPlayers = 0x790950, kLockout = 0x79094C;
constexpr uint32_t kOrder = 0x4FC888, kWeaponInfo = 0x4FA160, kWeaponInfoSize = 0x3C, kWeapons = 70;
constexpr uint32_t kPreferred = 0x1BAA70, kUsable = 0x1BA410, kTake = 0x1BA130, kRedrawHud = 0x142820;
constexpr uint32_t kHeld = 0x94;  // the entity's weapon

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

void TakeWeapon(X86Context& ctx) {
    uint32_t player = kernel::R32(kPlayer);
    if (!Valid(player, kPlayerSize) || kernel::R32(player + kSlot) != 0) return;
    int place = controls::TakeWeapon();
    if (!place || lockstep::Active()) return;
    // on foot and free to act, the game not paused
    if (!(kernel::R32(player + kState) & 0x480E) || (kernel::R32(player + kFlags) & 0x4000) ||
        int32_t(kernel::R32(kLockout)) > 100)
        return;
    uint32_t pause = kernel::R32(kPauseState);
    if (Valid(pause, 4) && (kernel::R32(pause) & 1)) return;
    uint32_t actor = kernel::R32(player + kBody);
    if (!Valid(actor, 0x80)) return;
    uint32_t entity = kernel::R32(actor + 0x7C);
    if (!Valid(entity, 0xC18) || kernel::R32(entity) != actor || kernel::R32(entity + 8) >= kernel::R32(kPlayers)) return;
    bool groups[kWeapons] = {};
    for (uint32_t i = 1; i < kWeapons; i++) {
        uint32_t weapon = kernel::R32(kOrder + 4 * i);
        if (weapon >= kWeapons) return;
        uint32_t group = kernel::R32(kWeaponInfo + kWeaponInfoSize * weapon);
        if (group >= kWeapons) return;
        if (groups[group] || !kernel::CallGuest(ctx, kPreferred, {entity, weapon}) ||
            !kernel::CallGuest(ctx, kUsable, {entity, weapon, 1}))
            continue;
        groups[group] = true;
        if (--place) continue;
        if (weapon != kernel::R32(entity + kHeld)) {
            kernel::CallGuest(ctx, kTake, {entity, weapon});
            kernel::CallGuest(ctx, kRedrawHud, {});
            LOG_FIRST(4, "weapon keys: weapon %u taken", weapon);
        }
        return;
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_001BF140);
X86_FUNC(sub_001BF140) {
    __imp__sub_001BF140(ctx, base);
    TakeWeapon(ctx);
}
