// Quit to Desktop, after the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc): a row
// at the end of the main menu and the pause menu opens a confirmation, Cancel (the default) or Quit to Desktop, built
// with the game's own menu calls; quitting closes the window as its close button does.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

#include "app/app.h"
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
// the main menu and the pause menu finish their rows from these places
constexpr uint32_t kFromMainMenu = 0x2D0EDF, kFromPause1 = 0x2F582F, kFromPause2 = 0x2F5879;
constexpr uint32_t kChosen = 0x18C;        // a menu's chosen row

// a menu descriptor (its builder first) and the strings, in guest memory
uint32_t g_storage = 0, g_parent = 0, g_previous = 0;
uint32_t g_open_cb = 0, g_build_cb = 0, g_select_cb = 0, g_back_cb = 0;
bool g_requested = false;
uint32_t Text() { return g_storage + 16; }
uint32_t Title() { return g_storage + 48; }
uint32_t Cancel() { return g_storage + 80; }

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

void QuitOpen(X86Context& ctx, uint8_t*);
void QuitBuild(X86Context& ctx, uint8_t*);
void QuitSelect(X86Context& ctx, uint8_t*);
void QuitBack(X86Context& ctx, uint8_t*);

bool Prepare() {
    if (g_storage) return true;
    g_storage = mem::AllocSystem(112);
    if (!g_storage) return false;
    std::memset(kernel::HostPtr(g_storage), 0, 112);
    g_open_cb = dispatch::HostThunk(QuitOpen);
    g_build_cb = dispatch::HostThunk(QuitBuild);
    g_select_cb = dispatch::HostThunk(QuitSelect);
    g_back_cb = dispatch::HostThunk(QuitBack);
    kernel::W32(g_storage, g_build_cb);
    std::memcpy(kernel::HostPtr(Text()), "Quit to Desktop", 16);
    std::memcpy(kernel::HostPtr(Title()), "Quit to Desktop?", 17);
    std::memcpy(kernel::HostPtr(Cancel()), "Cancel", 7);
    return true;
}

// (source menu): the confirmation replaces it
void QuitOpen(X86Context& ctx, uint8_t*) {
    uint32_t source = kernel::R32(ctx.esp + 4);
    if (Valid(source, 0x3C) && kernel::R32(source + 0x38) == 0 && Prepare()) {
        g_parent = kernel::R32(source + 0x2C);
        g_previous = kernel::R32(source + 4);
        g_requested = false;
        kernel::CallGuest(ctx, kShowMenu, {g_parent, g_storage});
    }
    kernel::Return(ctx, 0, 0);
}

// (menu): its title and rows
void QuitBuild(X86Context& ctx, uint8_t*) {
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

// back to the menu it came from
void QuitBack(X86Context& ctx, uint8_t*) {
    kernel::CallGuest(ctx, kShowMenu, {g_parent, g_previous});
    kernel::Return(ctx, 0, 0);
}

// (menu): Cancel goes back, Quit to Desktop closes the window
void QuitSelect(X86Context& ctx, uint8_t* base) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    if (Valid(menu, 0x190)) {
        uint32_t row = kernel::R32(menu + kChosen);
        if (row == 0) {
            kernel::CallGuest(ctx, kShowMenu, {g_parent, g_previous});
        } else if (row == 1 && !g_requested) {
            g_requested = PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0) != 0;
            if (g_requested) LOGR("quit: to the desktop from the menu");
        }
    }
    (void)base;
    kernel::Return(ctx, 0, 0);
}

// the main menu and pause menu get the row last, before their rows are laid out; the main menu's own callback takes
// ids 0-7 only, the new row has a callback of its own
void AddQuitRow(X86Context& ctx) {
    uint32_t caller = kernel::R32(ctx.esp), menu = kernel::R32(ctx.esp + 4);
    bool main_menu = caller == kFromMainMenu, paused = caller == kFromPause1 || caller == kFromPause2;
    if (!(main_menu || paused) || !Valid(menu, 0x3C) || kernel::R32(menu + 0x38) != 0 || !Prepare()) return;
    X86Context saved = ctx;
    kernel::CallGuest(ctx, kAddRow, {Text(), 8, main_menu ? 0x30000u : 0u, 0, g_open_cb, 0});
    ctx = saved;
}

}  // namespace

namespace game {
void AddCheatRow(X86Context& ctx);  // game/cheats.cpp: Invincible in the Cheats menu
}

X86_EXTERN_FUNC(__imp__sub_000771D0);
X86_FUNC(sub_000771D0) {
    AddQuitRow(ctx);
    game::AddCheatRow(ctx);
    __imp__sub_000771D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00077140);
X86_FUNC(sub_00077140) {
    AddQuitRow(ctx);
    __imp__sub_00077140(ctx, base);
}
