// Unlock Everything: a row under View Gallery in the Player menu (main menu, the player's statistics) opens a
// confirmation, Cancel (the default) or Unlock Everything, built with the game's own menu calls. Unlocking does what
// the game's debug cheat UNLOCK FEATURES does (sub_001CD200, from the button code at 0x4FCB58), for the profile the
// Player menu shows instead of the first one: the story completion masks (single and co-op, every difficulty) and the
// award masks all set, the 48 challenge and league grades platinum, the feature flags set. The profiles are then
// saved as the game saves them from its menus, and the Player menu comes back with the new totals.
#include <cstring>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // cpu/dispatch.cpp: a guest address that calls host code
}

namespace {

// the game's menu calls
constexpr uint32_t kShowMenu = 0x79B70;    // (parent, descriptor): the parent's child becomes a new menu
constexpr uint32_t kMenuTitle = 0x75B70;   // (flags, title, size, 0, back callback)
constexpr uint32_t kMenuLayout = 0x71B10;  // (1, 2.0f, 1, 1)
constexpr uint32_t kMenuStyle = 0x71C50;   // (flags)
constexpr uint32_t kAddRow = 0x79D60;      // (text, id, flags, 0, select callback, 0)
constexpr uint32_t kFinishMenu = 0x771D0;  // (menu): the rows laid out
constexpr uint32_t kSelect = 0x770E0;      // (menu, row index)
constexpr uint32_t kChosen = 0x18C;        // a menu's chosen row
// the Player menu adds View Gallery from here
constexpr uint32_t kFromPlayerMenu = 0x2D697A;
// profiles: the progress of a loaded profile (slot), or a scratch copy when there is none
constexpr uint32_t kProgress = 0x69900, kNoProgress = 0x6F11C0;
constexpr uint32_t kShownProfile = 0x78AD34, kProfileCount = 0x5236C4;
// (parent, next descriptor): the profiles saved; 0 when nothing was shown, the caller then shows the next menu
constexpr uint32_t kSaveProfiles = 0x193BB0;

// a menu descriptor (its builder first) and the strings, in guest memory
uint32_t g_storage = 0, g_parent = 0, g_previous = 0;
uint32_t g_open_cb = 0, g_build_cb = 0, g_select_cb = 0, g_back_cb = 0;
uint32_t Text() { return g_storage + 16; }
uint32_t Title() { return g_storage + 48; }
uint32_t Cancel() { return g_storage + 80; }

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

void UnlockOpen(X86Context& ctx, uint8_t*);
void UnlockBuild(X86Context& ctx, uint8_t*);
void UnlockSelect(X86Context& ctx, uint8_t*);
void UnlockBack(X86Context& ctx, uint8_t*);

bool Prepare() {
    if (g_storage) return true;
    g_storage = mem::AllocSystem(112);
    if (!g_storage) return false;
    std::memset(kernel::HostPtr(g_storage), 0, 112);
    g_open_cb = dispatch::HostThunk(UnlockOpen);
    g_build_cb = dispatch::HostThunk(UnlockBuild);
    g_select_cb = dispatch::HostThunk(UnlockSelect);
    g_back_cb = dispatch::HostThunk(UnlockBack);
    kernel::W32(g_storage, g_build_cb);
    std::memcpy(kernel::HostPtr(Text()), "Unlock Everything", 18);
    std::memcpy(kernel::HostPtr(Title()), "Unlock Everything?", 19);
    std::memcpy(kernel::HostPtr(Cancel()), "Cancel", 7);
    return true;
}

// the shown profile's progress all unlocked, as sub_001CD200(1) does for the first profile
bool Unlock(X86Context& ctx) {
    uint32_t slot = kernel::R32(kShownProfile), count = kernel::R32(kProfileCount);
    if (slot >= 16 || slot >= (count ? count : 1)) return false;
    uint32_t progress = kernel::CallGuest(ctx, kProgress, {slot});
    if (progress == kNoProgress || !Valid(progress, 0xC4C)) return false;
    for (uint32_t off = 0xBF4; off < 0xC4C; off += 4) kernel::W32(progress + off, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 48; i++) kernel::W32(progress + 0x49C + i * 0x18, 4);
    kernel::W32(progress + 0xBF0, 0xFFFFFFFF);
    LOGR("unlock: profile %u, everything unlocked", slot);
    return true;
}

void Leave(X86Context& ctx) { kernel::CallGuest(ctx, kShowMenu, {g_parent, g_previous}); }

// (source menu): the confirmation replaces it
void UnlockOpen(X86Context& ctx, uint8_t*) {
    uint32_t source = kernel::R32(ctx.esp + 4);
    if (Valid(source, 0x3C) && kernel::R32(source + 0x38) == 0 && Prepare()) {
        g_parent = kernel::R32(source + 0x2C);
        g_previous = kernel::R32(source + 4);
        kernel::CallGuest(ctx, kShowMenu, {g_parent, g_storage});
    }
    kernel::Return(ctx, 0, 0);
}

// (menu): its title and rows
void UnlockBuild(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    kernel::CallGuest(ctx, kMenuTitle, {0xA000, Title(), 0x425C0000, 0, g_back_cb});
    kernel::CallGuest(ctx, kMenuLayout, {1, 0x40000000, 1, 1});
    kernel::CallGuest(ctx, kMenuStyle, {0x26200});
    kernel::CallGuest(ctx, kAddRow, {Cancel(), 0, 0, 0, g_select_cb, 0});
    kernel::CallGuest(ctx, kAddRow, {Text(), 1, 0, 0, g_select_cb, 0});
    kernel::CallGuest(ctx, kFinishMenu, {menu});
    kernel::CallGuest(ctx, kSelect, {menu, 0});  // Cancel is the safe default
    kernel::Return(ctx, 0, 0);
}

// back to the Player menu
void UnlockBack(X86Context& ctx, uint8_t*) {
    Leave(ctx);
    kernel::Return(ctx, 0, 0);
}

// (menu): Cancel goes back, Unlock Everything unlocks and saves, then goes back
void UnlockSelect(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    if (Valid(menu, kChosen + 4)) {
        uint32_t row = kernel::R32(menu + kChosen);
        if (row == 1 && Unlock(ctx) && kernel::CallGuest(ctx, kSaveProfiles, {g_parent, g_previous})) {
            // the saving message shows the Player menu after
        } else if (row == 0 || row == 1) {
            Leave(ctx);
        }
    }
    kernel::Return(ctx, 0, 0);
}

}  // namespace

namespace game {
// called from the row hook (game/video_menu.cpp) after a row was added: the new row under View Gallery
void AddUnlockRow(X86Context& ctx, uint32_t caller) {
    if (caller != kFromPlayerMenu || !Prepare()) return;
    X86Context saved = ctx;
    kernel::CallGuest(ctx, kAddRow, {Text(), 0xFFFFFFFF, 0, 0, g_open_cb, 0});
    ctx = saved;
}
}  // namespace game
