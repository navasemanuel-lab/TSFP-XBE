// The mouse in the menus, after the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc):
// pointing at a row selects it, a click confirms it, a click on a choice's arrow steps it, a slider follows a held
// click, the footer's button prompts and the pause screen's tabs are buttons, the character carousel and the team
// chooser take clicks, the wheel moves the selection and the right button backs out. Everything goes through the
// menu's own action bits or the pad's newly pressed buttons, so callbacks, sounds, disabled rows and transitions stay
// the game's.
//
// A menu (0x23C bytes): +0x04 descriptor, +0x2C its container, +0x38 0 when it is the live one, +0x40 flags (8 a text
// page, 0x2000 modal), +0xC4 the actions this tick (low word pressed, high word repeat edge: 1 up, 2 down, 0x10
// confirm, 0x20 back, 0x10 << n footer button n), +0xC8 held actions, +0xE4..+0xF0 the clip rectangle, +0x128 four
// footer prompts of 16 bytes (button, ...), +0x16C its parent, +0x170 its child, +0x178 the first row, +0x198 the
// selected row, +0x1B0 3 when it takes input. A row: +0x00 type (4, 7, 8 can be chosen), +0x04 the next, +0x14 id,
// +0x20 caption, +0x28 flags (1 and 4 disabled or hidden), +0x78..+0x84 its rectangle, +0x88 its menu, +0xB4 1 a
// choice, +0xB8 style, +0xC8 the choice. Rectangles are pixels of the UI canvas at 0x4E8CF0 x 0x4E8CF4.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>

#include "common/mouse.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kCanvasW = 0x4E8CF0, kCanvasH = 0x4E8CF4;
constexpr uint32_t kFrontend = 0x79094C;     // above 100 in the frontend
constexpr uint32_t kPause = 0x761C48;        // the pause state: +0 bit 0 a pause or status menu up
constexpr uint32_t kPauseRoot = 0x761BBC;    // the pause screen's root menu: +0x170 the page shown
constexpr uint32_t kPauseBusy = 0x761BB8;
constexpr uint32_t kFrontRoot = 0x7613E4;    // the frontend's root menu
constexpr uint32_t kNetInput = 0x76B0FC;     // set: the menus read the network pads
constexpr uint32_t kMenuPlayer = 0x7BA9E0;   // the player a menu is for
constexpr uint32_t kTextAlign = 0x7E7260;    // 1 centred, 2 right
constexpr uint32_t kTabTable = 0x5247C4;     // 12 pause tabs of 12 bytes: +0 the page's descriptor
constexpr uint32_t kProfileMenu = 0x4E8D54;  // the profile screen's descriptor
constexpr uint32_t kSoundPlayer = 0x7BA16C, kSoundBank = 0x7BA178;

// game functions
constexpr uint32_t kMenuActive = 0x74760;    // (menu): it takes input now
constexpr uint32_t kSelectRow = 0x76EF0;     // (menu, row)
constexpr uint32_t kFooterEnabled = 0x76510; // (menu, slot)
constexpr uint32_t kMeasureText = 0x3FCF0;   // (&w, &h, text, flag)
constexpr uint32_t kMusicChanged = 0x45C60, kPreviewSound = 0x21780;
constexpr uint32_t kSensitivityPreviewA = 0x45390, kSensitivityPreviewB = 0x45820;
constexpr uint32_t kClosePause = 0x2E6D40;   // (0)
constexpr uint32_t kFirstTab = 0x2E49F0, kLastTab = 0x2E4A30, kShowPage = 0x79860;
constexpr uint32_t kCharacterPreview = 0x2C4310;  // (1), ECX character, EDI skin, EDX player

constexpr uint32_t kMusicVolume = 0x5236BC, kSoundVolume = 0x5236C0;  // 0-16
constexpr uint32_t kSensitivity = 0x7BA3DC;  // + player * 0x1E0, 0-1 in 0.05 steps

enum : uint32_t { kHover = 1, kClick = 2, kRightClick = 4, kWheelUp = 8, kWheelDown = 16 };
constexpr uint32_t kHighlight = 0x405A7F00;  // the footer's hover colour, RGB at the UI's half intensity

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
uint32_t R(uint32_t va) { return kernel::R32(va); }
uint32_t R8(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }
float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void W(uint32_t va, uint32_t v) { kernel::W32(va, v); }
void WF(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }
bool Finite(float a, float b, float c, float d) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

// the frontend or a pause screen
bool MenuScreen() {
    uint32_t pause = R(kPause);
    return int32_t(R(kFrontend)) > 100 || (Valid(pause, 4) && (R(pause) & 1));
}
// the menus take the mouse there, and anywhere the view is not being aimed: MapMaker's own menus and keyboard
bool MenuContext() { return MenuScreen() || !mouse::InPlay(); }

bool LiveMenu(uint32_t menu) { return Valid(menu, 0x23C) && R(menu + 0x38) == 0 && R(menu + 0x1B0) == 3; }
bool InClip(uint32_t menu, float x, float y) {
    return x >= F(menu + 0xE4) && x < F(menu + 0xEC) && y >= F(menu + 0xE8) && y < F(menu + 0xF0);
}

// guest stack space below the current frame, for a call's outputs and strings
struct Scratch {
    X86Context& ctx;
    X86Context saved;
    uint32_t base;
    Scratch(X86Context& c, uint32_t bytes) : ctx(c), saved(c) {
        base = (ctx.esp - bytes - 64) & ~15u;
        ctx.esp = base - 64;
    }
    ~Scratch() { ctx = saved; }
};

// a string's width and height in the current font
float Measure(X86Context& ctx, uint32_t text, uint32_t flag, float& h) {
    Scratch s(ctx, 16);
    kernel::CallGuest(ctx, kMeasureText, {s.base, s.base + 4, text, flag});
    h = F(s.base + 4);
    return F(s.base);
}
float MeasurePrefix(X86Context& ctx, uint32_t text, uint32_t length, float& h) {
    Scratch s(ctx, 640);
    std::memcpy(kernel::HostPtr(s.base + 32), kernel::HostPtr(text), length);
    *static_cast<char*>(kernel::HostPtr(s.base + 32 + length)) = 0;
    kernel::CallGuest(ctx, kMeasureText, {s.base, s.base + 4, s.base + 32, 1});
    h = F(s.base + 4);
    return F(s.base);
}

// the pointer on the UI canvas
bool CanvasPoint(const mouse::Pointer& p, float& x, float& y) {
    float w = float(R(kCanvasW)), h = float(R(kCanvasH));
    if (!p.valid || !(w > 0) || !(h > 0)) return false;
    x = p.x * w;
    y = p.y * h;
    return true;
}

// hover, fresh clicks and the wheel; a still pointer leaves the keyboard's selection alone, a held button never
// clicks the next screen
struct Tracker {
    uint32_t menu = 0;
    float x = 0, y = 0;
    bool valid = false, left = false, right = false, blocked = false, owns = false, scrolling = false;
    int wheel_pending = 0;
    uint64_t wheel_at = 0, pointer_at = 0;
};
Tracker g, g_carousel;

uint32_t Actions(Tracker& s, uint32_t menu, mouse::Pointer p) {
    uint64_t now = NowMs();
    uint32_t result = 0;
    if (menu != s.menu || !p.valid) {
        s.blocked = p.left || p.right;
        s.valid = false;
        s.wheel_pending = 0;
        s.scrolling = false;
        p.wheel = 0;
    }
    if (!p.valid || !menu) s.owns = false;
    if (!p.left && !p.right) s.blocked = false;
    if (p.valid && menu) {
        bool moved = s.valid && (p.x != s.x || p.y != s.y);
        if (moved) s.pointer_at = now;
        if (s.scrolling && now - s.wheel_at >= 250 && now - s.pointer_at >= 100) s.scrolling = false;
        if (moved && !s.scrolling) result |= kHover;
        if (!s.blocked && p.left && !s.left) result |= kClick;
        if (!s.blocked && p.right && !s.right) result |= kRightClick;
        if (p.wheel) {
            if ((p.wheel > 0) != (s.wheel_pending > 0)) s.wheel_pending = 0;
            s.wheel_pending = std::clamp(s.wheel_pending + std::clamp(p.wheel, -8, 8), -8, 8);
        }
        if (result & (kClick | kRightClick)) {
            s.wheel_pending = 0;
            s.scrolling = false;
        } else if (s.wheel_pending) {
            result = s.wheel_pending > 0 ? kWheelUp : kWheelDown;
            s.scrolling = true;
            s.wheel_at = s.pointer_at = now;
            s.wheel_pending += s.wheel_pending > 0 ? -1 : 1;
        }
    }
    s.menu = menu;
    s.x = p.x;
    s.y = p.y;
    s.valid = p.valid;
    s.left = p.left;
    s.right = p.right;
    return result;
}

// One dispatch of a menu's actions. Some callbacks look at pad 1's newly pressed buttons to see who confirmed, so a
// click is a pad press for the dispatch's length. The pointer is sampled once, by the first hook that needs it.
struct Dispatch {
    uint32_t menu = 0, pressed = 0, injected = 0, pressed_va = 0;
    mouse::Pointer pointer;
    uint32_t actions = 0;
    bool sampled = false;
    uint32_t close_pause = 0, tab_menu = 0, tab_target = 0;
};
Dispatch* g_dispatch = nullptr;

void PressPad(Dispatch& d, uint32_t buttons) {
    if (!d.injected) {
        d.pressed_va = R(kNetInput) ? 0x6B94D8 : 0x6B93E0;
        d.pressed = R(d.pressed_va);
    }
    d.injected |= buttons;
    W(d.pressed_va, R(d.pressed_va) | buttons);
}
void UnpressPad(Dispatch& d) {
    if (d.injected) W(d.pressed_va, (R(d.pressed_va) & ~d.injected) | (d.pressed & d.injected));
}

// ---- pause screen: tabs, and closing it ----

bool CanClosePause(uint32_t menu) {
    uint32_t pause = R(kPause), root = R(kPauseRoot);
    return int32_t(R(kFrontend)) <= 100 && Valid(pause, 4) && (R(pause) & 7) == 1 && int32_t(R(kPauseBusy)) <= 0 &&
           Valid(root, 0x23C) && R(root + 0x170) == menu && Valid(menu, 0x23C) && R(menu + 0x1B0) == 3 &&
           !R(menu + 0x1C0) && !(R(menu + 0x40) & 0x2000);
}

struct Tab {
    uint32_t menu, source, target;
    float x, y, w, h;
    uint64_t time;
};
Tab g_tabs[12];

int TabIndex(uint32_t descriptor) {
    for (int i = 0; i < 12; i++)
        if (descriptor && R(kTabTable + i * 12) == descriptor) return i;
    return -1;
}
bool TabsActive(X86Context& ctx, uint32_t menu) {
    uint32_t pause = R(kPause), root = R(kPauseRoot);
    return int32_t(R(kFrontend)) <= 100 && Valid(pause, 0x268) && (R(pause) & 7) == 1 && int32_t(R(kPauseBusy)) <= 0 &&
           Valid(root, 0x23C) && R(root + 0x170) == menu && LiveMenu(menu) && !(R(menu + 0x40) & 0x2000) &&
           !((R(0x789640) | R(0x7894FC)) & 1) && TabIndex(R(menu + 4)) >= 0 &&
           kernel::CallGuest(ctx, kMenuActive, {menu});
}
bool TabContains(const Tab& t, uint32_t menu, float x, float y) {
    return t.menu == menu && t.source == R(menu + 4) && t.target != t.source && NowMs() - t.time <= 100 && x >= t.x &&
           x < t.x + t.w && y >= t.y && y < t.y + t.h;
}
uint32_t TabHit(X86Context& ctx, uint32_t menu, float x, float y) {
    if (!TabsActive(ctx, menu)) return 0;
    for (int i = 0; i < 12; i++)
        if (TabContains(g_tabs[i], menu, x, y)) return i + 1;
    return 0;
}
void ChangeTab(X86Context& ctx, uint32_t menu, uint32_t target) {
    if (!target || target > 12 || !TabsActive(ctx, menu)) return;
    const Tab& t = g_tabs[target - 1];
    if (t.menu != menu || t.source != R(menu + 4) || NowMs() - t.time > 100) return;
    X86Context saved = ctx;
    uint32_t player = R(kMenuPlayer), pause = R(kPause);
    W(kMenuPlayer, 0);
    // the previous and next tab helpers' transition state, then the game's own page change and animation
    int index = int(target) - 1, old = TabIndex(t.source);
    uint32_t first = kernel::CallGuest(ctx, kFirstTab, {}), last = kernel::CallGuest(ctx, kLastTab, {});
    W(pause + 0x264, index == int(first) ? 1 : index == int(last) ? 2 : 0);
    WF(pause + (index < old ? 0x25C : 0x260), 1.0f);
    kernel::CallGuest(ctx, kShowPage, {R(kPauseRoot), t.target, 0xFFFFFFFF});
    W(kMenuPlayer, player);
    ctx = saved;
}

// ---- footer prompts ----

struct Footer {
    uint32_t menu, descriptor[4];
    float x, y, w, h;
    uint64_t time;
};
Footer g_footer[4];
uint32_t g_footer_drawing = 0;

bool FooterContains(X86Context& ctx, const Footer& f, uint32_t menu, uint32_t slot, float x, float y) {
    if ((R(menu + 0x40) & 0x04000000) || f.menu != menu || NowMs() - f.time > 100 ||
        std::memcmp(f.descriptor, kernel::HostPtr(menu + 0x128 + slot * 16), 16) || f.descriptor[0] > 3 ||
        f.descriptor[2] != 0 || !(f.w > 0) || !(f.h > 0))
        return false;
    return x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h &&
           kernel::CallGuest(ctx, kFooterEnabled, {menu, slot}) != 0;
}
int FooterHit(X86Context& ctx, uint32_t menu, float x, float y) {
    X86Context saved = ctx;
    int hit = -1;
    for (uint32_t i = 0; i < 4 && hit < 0; i++)
        if (FooterContains(ctx, g_footer[i], menu, i, x, y)) hit = int(g_footer[i].descriptor[0]);
    ctx = saved;
    return hit;
}

// the profile screen's prompts: one centred line of button tokens (\v1-\v4) and captions
struct ProfilePrompt {
    uint32_t menu, row, caption;
    float x, y, w, h;
    uint64_t time;
};
ProfilePrompt g_profile[4];

bool ProfileActive(X86Context& ctx, uint32_t menu) {
    return int32_t(R(kFrontend)) > 100 && LiveMenu(menu) && R(menu + 4) == kProfileMenu &&
           kernel::CallGuest(ctx, kMenuActive, {menu});
}
int ProfileHit(X86Context& ctx, uint32_t menu, float x, float y) {
    X86Context saved = ctx;
    int hit = -1;
    if (ProfileActive(ctx, menu)) {
        for (int i = 0; i < 4; i++) {
            const ProfilePrompt& f = g_profile[i];
            if (f.menu != menu || !Valid(f.row, 0x10C) || R(f.row + 0x88) != menu || R(f.row + 0x20) != f.caption ||
                (R(f.row + 0x28) & 0x21) || (R(f.row + 0x2C) & 0x20) || NowMs() - f.time > 100)
                continue;
            if (x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h) {
                hit = i;
                break;
            }
        }
    }
    ctx = saved;
    return hit;
}

void NoteProfileText(X86Context& ctx) {
    uint32_t frame = ctx.esp, menu = ctx.edi, row = ctx.esi, text = R(frame + 12);
    if (R(frame) != 0x763C3 || !Valid(row, 0x10C) || R(row + 0x88) != menu ||
        (int32_t(R(row + 0x14)) != -4 && int32_t(R(row + 0x14)) != -5) || !Valid(text, 512))
        return;
    X86Context saved = ctx;
    if (!ProfileActive(ctx, menu)) {
        ctx = saved;
        return;
    }
    ctx = saved;
    const char* str = static_cast<const char*>(kernel::HostPtr(text));
    const char* end = static_cast<const char*>(std::memchr(str, 0, 512));
    if (!end || std::memchr(str, '\n', size_t(end - str))) return;
    uint32_t length = uint32_t(end - str), starts[4], buttons[4], count = 0;
    for (uint32_t i = 0; i + 1 < length; i++) {
        if (str[i] == '\v' && str[i + 1] >= '1' && str[i + 1] <= '4') {
            static const uint32_t kMap[] = {1, 0, 3, 2};
            if (count == 4) return;
            starts[count] = i;
            buttons[count++] = kMap[str[i + 1] - '1'];
            i++;
        }
    }
    if (!count) return;
    float height, ignored, width = MeasurePrefix(ctx, text, length, height);
    float origin = F(frame + 4), y = F(frame + 8);
    uint32_t align = R(kTextAlign);
    if (align == 1) origin -= width * 0.5f;
    else if (align == 2) origin -= width;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t stop = i + 1 < count ? starts[i + 1] : length;
        while (stop > starts[i] && str[stop - 1] == ' ') stop--;
        float a = origin + MeasurePrefix(ctx, text, starts[i], ignored);
        float b = origin + MeasurePrefix(ctx, text, stop, ignored);
        g_profile[buttons[i]] = {menu, row, R(row + 0x20), a, y, b - a, height, NowMs()};
    }
}

// DrawText(x, y, text, colour) at a footer, tab or profile prompt call: its hit area, true when the pointer is on it
bool NoteText(X86Context& ctx) {
    uint32_t caller = R(ctx.esp);
    if (caller == 0x763C3) {
        NoteProfileText(ctx);
        return false;
    }
    mouse::Pointer p = mouse::MenuPointer(false);
    float px, py;
    bool pointed = CanvasPoint(p, px, py);
    if (caller == 0x2E51F9) {  // a pause tab: the drawing loop's one-based index is a local
        uint32_t index = R(ctx.esp + 0x2C);
        if (index < 1 || index > 12) return false;
        uint32_t root = R(kPauseRoot);
        if (!Valid(root, 0x23C)) return false;
        uint32_t menu = R(root + 0x170);
        X86Context saved = ctx;
        bool active = TabsActive(ctx, menu);
        ctx = saved;
        if (!active) return false;
        Tab& t = g_tabs[index - 1];
        float h, w = Measure(ctx, R(ctx.esp + 12), 0, h);
        t = {menu, R(menu + 4), R(kTabTable + (index - 1) * 12), F(ctx.esp + 4), F(ctx.esp + 8), w, h, NowMs()};
        return pointed && TabContains(t, menu, px, py);
    }
    uint32_t menu = g_footer_drawing, slot = ctx.esi;
    if (!menu || slot >= 4 || (caller != 0x767E4 && caller != 0x2D91B9 && caller != 0x2F4108)) return false;
    // the themed painters pack the visible prompts; the generic one keeps the slots
    if (caller != 0x767E4) {
        uint32_t visible = 0, s = 0;
        for (; s < 4; s++) {
            uint32_t entry = menu + 0x128 + s * 16;
            if (R(entry) != 0xFFFFFFFF && R(entry + 8) != 2 && visible++ == slot) break;
        }
        if (s == 4) return false;
        slot = s;
    }
    Footer& f = g_footer[slot];
    f.menu = menu;
    std::memcpy(f.descriptor, kernel::HostPtr(menu + 0x128 + slot * 16), 16);
    f.x = F(ctx.esp + 4);
    f.y = F(ctx.esp + 8);
    f.w = Measure(ctx, R(ctx.esp + 12), 0, f.h);
    f.time = NowMs();
    if (!pointed || !MenuContext() || R(menu + 0x1B0) != 3) return false;
    X86Context saved = ctx;
    bool hover = kernel::CallGuest(ctx, kMenuActive, {menu}) && FooterContains(ctx, f, menu, slot, px, py);
    ctx = saved;
    return hover;
}

// ---- choice arrows ----

struct Arrow {
    uint32_t menu, descriptor, row, id, choice, direction;
    float left, top, right, bottom, rect[4];
    uint64_t time;
};
Arrow g_arrows[256];
uint32_t g_choice_drawing = 0;  // a custom choice row painter's row

uint32_t ArrowHit(uint32_t menu, float x, float y, uint32_t& row_out) {
    if (!InClip(menu, x, y)) return 0;
    uint64_t now = NowMs();
    for (const Arrow& a : g_arrows) {
        uint32_t row = a.row;
        if (a.menu != menu || a.descriptor != R(menu + 4) || now - a.time > 100 || !Valid(row, 0x10C) ||
            R(row + 0x88) != menu || R(row + 0x14) != a.id || R(row + 0xB4) != 1 || R(row + 0xC8) != a.choice ||
            (R(row + 0x28) & 5) || (R(row + 0xB8) & 0x400000) ||
            std::memcmp(a.rect, kernel::HostPtr(row + 0x78), sizeof a.rect))
            continue;
        if (x >= a.left && x < a.right && y >= a.top && y < a.bottom && y >= a.rect[1] && y < a.rect[3]) {
            row_out = row;
            return a.direction;
        }
    }
    return 0;
}

// ---- team chooser (multiplayer pause page) ----

struct TeamArrow {
    uint32_t menu, player, team;
    float left, top, right, bottom;
    uint64_t time;
};
TeamArrow g_team_arrows[2];
uint32_t g_team_drawing = 0;

uint32_t TeamChild(uint32_t parent) {
    if (!LiveMenu(parent) || R(parent + 4) != 0x524AD8) return 0;
    uint32_t child = R(parent + 0x170);
    return Valid(child, 0x23C) && R(child + 4) == 0x524AC8 && R(child + 0x1B0) == 3 && !R(child + 0x170) ? child : 0;
}
uint32_t TeamPlayer() {
    uint32_t list = R(0x774028);
    if (!Valid(list, 0x24)) return 0;
    uint32_t count = R(list + 0x20);
    if (count < 1 || count > 4) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t player = R(list + 12 + i * 4);
        if (Valid(player, 0x124) && R(player + 0x3C) == 0) return player;
    }
    return 0;
}

void NoteArrow(X86Context& ctx, uint32_t direction) {
    uint32_t row = ctx.esi, caller = R(ctx.esp);
    float l = F(ctx.esp + 4), t = F(ctx.esp + 8), r = F(ctx.esp + 12), b = F(ctx.esp + 16);
    if (!Finite(l, t, r, b) || r <= l || b <= t || r - l > 10000 || b - t > 10000) return;
    float margin = (b - t) * 0.4f;  // a little more to aim at, still inside the row
    if (g_team_drawing && ((direction == 4 && caller == 0x2E4EFB) || (direction == 8 && caller == 0x2E4F48))) {
        uint32_t player = TeamPlayer();
        if (player && !R(player + 0x11C))
            g_team_arrows[direction == 8] = {g_team_drawing, player, R(player + 0x48), l - margin, t - margin,
                                             r + margin, b + margin, NowMs()};
        return;
    }
    if ((direction == 4 && (caller == 0x19F174 || caller == 0x360EDD)) ||
        (direction == 8 && (caller == 0x19F1D8 || caller == 0x360F74)))
        row = g_choice_drawing;
    else if (!((direction == 4 && caller == 0x7BA56) || (direction == 8 && caller == 0x7BA30)))
        return;
    if (!Valid(row, 0x10C)) return;
    uint32_t menu = R(row + 0x88);
    if (!Valid(menu, 0x23C) || R(row + 0xB4) != 1 || (R(row + 0x28) & 5) || (R(row + 0xB8) & 0x400000)) return;
    Arrow* slot = &g_arrows[0];
    for (Arrow& a : g_arrows) {
        if (a.row == row && a.direction == direction) {
            slot = &a;
            break;
        }
        if (a.time < slot->time) slot = &a;
    }
    *slot = {menu, R(menu + 4), row, R(row + 0x14), R(row + 0xC8), direction, l - margin, t - margin, r + margin,
             b + margin, {}, NowMs()};
    std::memcpy(slot->rect, kernel::HostPtr(row + 0x78), sizeof slot->rect);
}

// ---- sliders: music, sound effects, controller sensitivity, and the bars of our own menus ----

enum { kMusic, kSound, kStickSensitivity, kBar, kSliderCount = kBar + 4 };
struct Slider {
    uint32_t menu, descriptor, row, id, player;
    float x, y, w, h;
    uint64_t time;
};
Slider g_sliders[kSliderCount];
int g_drag = -1;
Slider g_drag_owner;
uint32_t g_sensitivity_menu = 0, g_sensitivity_row = 0;
void (*g_bar_set[kSliderCount - kBar])(uint32_t id, float at);  // a bar's setter: its row's id, 0 to 1 along it

void NoteSlider(int kind, uint32_t menu, uint32_t row, float x, float y, float w, float h) {
    Slider& s = g_sliders[kind];
    s = {};
    if (!Valid(menu, 0x23C) || !Valid(row, 0x10C) || R(row + 0x88) != menu || !Finite(x, y, w, h) ||
        std::fabs(x) > 1e5f || std::fabs(y) > 1e5f || w <= 0 || h <= 0 || w > 1e5f || h > 1e5f)
        return;
    s = {menu, R(menu + 4), row, R(row + 0x14), R(kMenuPlayer), x, y, w, h, NowMs()};
}

// the audio painters (menu, row, rectangle) place their bars at these offsets in both layouts
void NoteAudioBars(X86Context& ctx, bool compact) {
    uint32_t menu = R(ctx.esp + 4), row = R(ctx.esp + 8), rect = R(ctx.esp + 12);
    if (!Valid(row, 0x10C) || !Valid(rect, 16)) return;
    uint32_t id = R(row + 0x14);
    if (id != 1 && id != 3) return;
    float l = F(rect), t = F(rect + 4), r = F(rect + 8), b = F(rect + 12);
    g_sliders[id == 3 ? kSound : kMusic] = {};
    if (!Finite(l, t, r, b) || std::fabs(l) > 1e5f || std::fabs(r) > 1e5f || r - l <= 65 || b <= t) return;
    NoteSlider(id == 3 ? kSound : kMusic, menu, row, float(int(l + 45)), float(int(t + (b - t) * (compact ? 0.65f : 0.5f))),
               float(int(r - l - 65)), compact ? 10.0f : 20.0f);
}

uint32_t SliderStep(float x, const Slider& s, uint32_t maximum) {
    float f = (x - s.x) / s.w;
    if (!std::isfinite(f) || f <= 0) return 0;
    if (f >= 1) return maximum;
    return uint32_t(std::floor(f * float(maximum) + 0.5f));
}

void SetSlider(X86Context& ctx, int kind, float x) {
    const Slider& s = g_sliders[kind];
    X86Context saved = ctx;
    if (kind >= kBar) {
        if (auto set = g_bar_set[kind - kBar]) set(R(s.row + 0x14), std::clamp((x - s.x) / s.w, 0.0f, 1.0f));
    } else if (kind == kStickSensitivity) {
        uint32_t va = kSensitivity + s.player * 0x1E0;
        float v = float(SliderStep(x, s, 20)) * 0.05f;
        if (F(va) != v) {
            WF(va, v);
            // the sensitivity row's own preview; Accept and Cancel still own saving
            if (R(kSoundPlayer) && Valid(R(kSoundBank), 12)) {
                uint32_t preview = R(R(kSoundBank) + 8);
                Scratch sc(ctx, 0x30);
                kernel::CallGuest(ctx, kSensitivityPreviewA, {sc.base});
                W(sc.base, preview);
                kernel::CallGuest(ctx, kSensitivityPreviewB, {sc.base});
            }
        }
    } else {
        uint32_t v = SliderStep(x, s, 16), va = kind == kMusic ? kMusicVolume : kSoundVolume;
        if (R(va) != v) {
            W(va, v);
            if (kind == kMusic) kernel::CallGuest(ctx, kMusicChanged, {});
            if (R(kSoundPlayer) && Valid(R(kSoundBank), 12)) kernel::CallGuest(ctx, kPreviewSound, {R(R(kSoundBank) + 8)});
        }
    }
    ctx = saved;
}

bool SliderLive(const Slider& s, uint32_t menu) {
    return s.menu == menu && s.descriptor == R(menu + 4) && s.player == R(kMenuPlayer) && NowMs() - s.time <= 100 &&
           Valid(s.row, 0x10C) && R(s.row + 0x88) == menu && R(s.row + 0x14) == s.id && !(R(s.row + 0x28) & 5) &&
           s.w > 0 && s.h > 0;
}

// a held click on a bar sets it and keeps it, wherever the pointer goes, until released
bool DragSlider(X86Context& ctx, uint32_t menu, const mouse::Pointer& p, uint32_t actions, float x, float y) {
    if (!p.left || p.right || g.blocked) g_drag = -1;
    int kind = -1;
    for (int i = 0; i < kSliderCount; i++) {
        const Slider& s = g_sliders[i];
        if (!SliderLive(s, menu)) continue;
        if (g_drag == i && g_drag_owner.menu == s.menu && g_drag_owner.id == s.id && g_drag_owner.player == s.player) {
            kind = i;
            break;
        }
        if ((actions & kClick) && !p.right && x >= s.x && x <= s.x + s.w && y >= s.y && y < s.y + s.h &&
            InClip(menu, x, y)) {
            g_drag = kind = i;
            g_drag_owner = s;
            break;
        }
    }
    if (kind < 0) {
        g_drag = -1;
        return false;
    }
    g.owns = true;
    g.wheel_pending = 0;
    g.scrolling = false;
    W(menu + 0xC4, R(menu + 0xC4) & ~0x001F001Fu);
    W(menu + 0xC8, R(menu + 0xC8) & ~0x1Fu);
    if (R(menu + 0x198) != g_sliders[kind].row) {
        X86Context saved = ctx;
        kernel::CallGuest(ctx, kSelectRow, {menu, g_sliders[kind].row});
        ctx = saved;
    }
    SetSlider(ctx, kind, x);
    return true;
}

}  // namespace

namespace game {
// a bar drawn this frame in a menu of ours (menu, row, its rectangle on the UI canvas): held clicks on it go to set
void NoteMenuBar(uint32_t menu, uint32_t row, float x, float y, float w, float h, void (*set)(uint32_t id, float at)) {
    int slot = kBar;
    for (int i = kBar; i < kSliderCount; i++) {
        if (g_sliders[i].row == row) {
            slot = i;
            break;
        }
        if (g_sliders[i].time < g_sliders[slot].time) slot = i;
    }
    g_bar_set[slot - kBar] = set;
    NoteSlider(slot, menu, row, x, y, w, h);
}
}  // namespace game

namespace {

// ---- character carousel ----

constexpr uint32_t kCarouselMenu = 0x5237E0, kCarouselContainer = 0x5237F0;
struct Portrait {
    uint32_t menu, group, variant, character, skin;
    float l, t, r, b;
    uint64_t time;
};
Portrait g_portraits[64];
uint32_t g_portrait_count = 0, g_carousel_drawing = 0;

bool CarouselActive(uint32_t menu) {
    return Valid(menu, 0x23C) && R(menu + 4) == kCarouselMenu && R(menu + 0x38) == 0 && R(menu + 0x1B0) == 3 &&
           !R(menu + 0x170) && R8(0x75FCF4) == 2;
}
bool CharacterIndex(uint32_t group, uint32_t variant, uint32_t& index) {
    uint32_t groups = R(0x78AC98), count = R(0x78AC94);
    if (!groups || groups > 64 || count > 150 || group >= groups || variant >= R8(0x78ACDC + group)) return false;
    index = R8(0x78AC9C + group) + variant;
    return index < count;
}
uint32_t Skin(uint32_t index) { return uint32_t(int32_t(int8_t(R8(0x78ABD0 + index)))); }

// the carousel's portrait quads, from DrawQuad's twelve arguments and its caller's locals
void NoteQuad(X86Context& ctx) {
    uint32_t menu = g_carousel_drawing, caller = R(ctx.esp), sp = ctx.esp;
    if (!menu || (caller != 0x2C5B72 && caller != 0x2C5C32) || g_portrait_count >= 64 ||
        !((R(sp + 36) | R(sp + 40) | R(sp + 44) | R(sp + 48)) & 0xFF000000))
        return;
    uint32_t group = R(sp + 0x5C), variant, index;
    if (group >= 64) return;
    if (caller == 0x2C5B72) {
        int v = int32_t(R(sp + 0x88)), count = int(R8(0x78ACDC + group));
        if (!count || v < -1 || v > count) return;
        variant = uint32_t(v < 0 ? v + count : v >= count ? v - count : v);
    } else {
        variant = R(0x78A148 + group * 4);
    }
    if (!CharacterIndex(group, variant, index)) return;
    float l = F(sp + 4), t = F(sp + 8), r = F(sp + 12), b = F(sp + 16);
    if (!Finite(l, t, r, b) || r <= l || b <= t) return;
    l = std::max(l, F(menu + 0xE4));
    t = std::max(t, F(menu + 0xE8));
    r = std::min(r, F(menu + 0xEC));
    b = std::min(b, F(menu + 0xF0));
    if (r <= l || b <= t) return;
    g_portraits[g_portrait_count++] = {menu, group, variant, R(0x78A720 + index * 4), Skin(index), l, t, r, b, NowMs()};
}

void SelectCharacter(X86Context& ctx, uint32_t group, uint32_t variant) {
    uint32_t index;
    if (!CharacterIndex(group, variant, index)) return;
    if (R(0x78A248) == group && R(0x78A148 + group * 4) == variant) return;
    W(0x78A248, group);
    W(0x78A148 + group * 4, variant);
    WF(0x78A284, float(variant));
    X86Context saved = ctx;
    ctx.ecx = R(0x78A720 + index * 4);
    ctx.edi = Skin(index);
    ctx.edx = 0;
    kernel::CallGuest(ctx, kCharacterPreview, {1});
    ctx = saved;
    W(0x78A258, 1);  // finish the model preview's transition
}

void CarouselPointer(X86Context& ctx, Dispatch& d) {
    if (R(d.menu + 4) != kCarouselContainer || !(d.actions & (kClick | kWheelUp | kWheelDown))) return;
    float x, y;
    if (!CanvasPoint(d.pointer, x, y)) return;
    for (uint32_t i = 0; i < g_portrait_count; i++) {
        const Portrait& p = g_portraits[i];
        uint32_t index;
        if (!CarouselActive(p.menu) || NowMs() - p.time > 100 || R(p.menu + 0x2C) != d.menu ||
            R(d.menu + 0x16C) != p.menu || R(d.menu + 0x170) || !CharacterIndex(p.group, p.variant, index) ||
            R(0x78A720 + index * 4) != p.character || Skin(index) != p.skin)
            continue;
        if (d.actions & (kWheelUp | kWheelDown)) {
            uint32_t count = R(0x78AC98), group = R(0x78A248);
            if (group >= count) return;
            group = (d.actions & kWheelUp) ? (group ? group - 1 : count - 1) : (group + 1) % count;
            SelectCharacter(ctx, group, R(0x78A148 + group * 4));
            d.actions = 0;
            return;
        }
        if (x >= p.l && x < p.r && y >= p.t && y < p.b) {
            SelectCharacter(ctx, p.group, p.variant);  // a preview; Select confirms
            d.actions = 0;
            return;
        }
    }
}

// the carousel's container fails the row menus' active test: it is checked through its root instead, sampled once
// before either tick, its Confirm and Back kept as pad presses through the nested callbacks
void CarouselInput(X86Context& ctx, Dispatch& d) {
    if (!LiveMenu(d.menu) || R(d.menu + 4) != kCarouselContainer || !MenuContext()) return;
    uint32_t root = R(kFrontRoot);
    if (!Valid(root, 0x23C) || R(root + 0x170) != d.menu || R(d.menu + 0x170)) return;
    mouse::InMenu();
    d.pointer = mouse::MenuPointer(true);
    d.actions = Actions(g_carousel, d.menu, d.pointer);
    d.sampled = true;
    CarouselPointer(ctx, d);
    float x, y;
    if ((d.actions & kClick) && CanvasPoint(d.pointer, x, y)) {
        int footer = FooterHit(ctx, d.menu, x, y);
        if (footer == 0 || footer == 1) PressPad(d, footer ? 0x120 : 0x80);
    }
    if (d.actions & kRightClick) PressPad(d, 0x120);
    d.actions = 0;
}

// ---- rows ----

uint32_t g_trace_hit = 0;

uint32_t RowHit(uint32_t menu, float x, float y) {
    if (!InClip(menu, x, y)) return 0;
    uint32_t row = R(menu + 0x178);
    for (int n = 0; row && n < 256 && Valid(row, 0x10C); n++, row = R(row + 4)) {
        uint32_t type = R(row), flags = R(row + 0x28);
        if ((type == 4 || type == 7 || type == 8) && !(flags & 5) && x >= F(row + 0x78) && x < F(row + 0x80) &&
            y >= F(row + 0x7C) && y < F(row + 0x84))
            return row;
    }
    return 0;
}

// ---- typing on the on-screen keyboards (name entry, MapMaker's names) ----

// a row's caption (+0x20) when it is a plain string
std::string RowText(uint32_t row) {
    uint32_t text = R(row + 0x20);
    if (!Valid(text, 1)) return {};
    std::string s;
    for (uint32_t i = 0; i < 32 && Valid(text + i, 1); i++) {
        uint8_t c = R8(text + i);
        if (!c) return s;
        if (c < 0x20) return {};
        s += char(c);
    }
    return {};
}

bool SameText(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

// a menu with a key row per character
bool KeyboardMenu(uint32_t menu) {
    static uint32_t last_menu = 0;
    static int last_keys = -1;
    int keys = 0;
    uint32_t first = R(menu + 0x178), row = first;
    for (int n = 0; row && n < 256 && Valid(row, 0x10C); n++, row = R(row + 4))
        if (!(R(row + 0x28) & 4) && RowText(row).size() == 1) keys++;
    bool keyboard = keys >= 20;
    bool changed = menu != last_menu || keys != last_keys;
    last_menu = menu;
    last_keys = keys;
    static const bool trace = std::getenv("XBE_TRACE_MENU") != nullptr;
    if (trace && changed) {
        bool cached = keyboard;
        LOGR("menu: %08X desc %08X keyboard %d (%d key rows)", menu, R(menu + 4), cached, keys);
        row = first;
        for (int n = 0; row && n < 256 && Valid(row, 0x10C); n++, row = R(row + 4))
            LOGR("menu:   row %08X type %u id %d flags %X at %.0f,%.0f-%.0f,%.0f '%s'", row, R(row), int(R(row + 0x14)),
                 R(row + 0x28), F(row + 0x78), F(row + 0x7C), F(row + 0x80), F(row + 0x84), RowText(row).c_str());
    }
    return keyboard;
}

uint32_t FindKey(uint32_t menu, int c, bool exact) {
    uint32_t row = R(menu + 0x178);
    for (int n = 0; row && n < 256 && Valid(row, 0x10C); n++, row = R(row + 4)) {
        if (R(row + 0x28) & 5) continue;
        std::string t = RowText(row);
        if (c == '\b' && (SameText(t, "Del") || SameText(t, "Delete"))) return row;
        if (c == '\r' && (SameText(t, "Done") || SameText(t, "OK"))) return row;
        if (c == ' ' && SameText(t, "Space")) return row;
        if (t.size() == 1 && (exact ? uint8_t(t[0]) == c : std::tolower(uint8_t(t[0])) == std::tolower(c))) return row;
    }
    return 0;
}

// one typed character a tick: its key row selected and confirmed; a letter in the other case presses Shift first
bool Typing(X86Context& ctx, uint32_t menu) {
    static int pending = 0, shifted = 0;
    int c = pending ? pending : mouse::TakeChar();
    pending = 0;
    if (!c) return false;
    uint32_t row = FindKey(menu, c, true);
    if (!row && std::isalpha(c) && shifted != c) {
        if (FindKey(menu, c, false)) {
            uint32_t shift = 0, r = R(menu + 0x178);
            for (int n = 0; r && n < 256 && Valid(r, 0x10C) && !shift; n++, r = R(r + 4))
                if (SameText(RowText(r), "Shift")) shift = r;
            if (shift) {
                pending = shifted = c;
                row = shift;
            }
        }
    }
    if (!pending) shifted = 0;
    if (!row) row = FindKey(menu, c, false);
    if (!row) return false;
    if (R(menu + 0x198) != row) {
        X86Context saved = ctx;
        kernel::CallGuest(ctx, kSelectRow, {menu, row});
        ctx = saved;
    }
    if (g_dispatch && g_dispatch->menu == menu && !g_dispatch->injected) PressPad(*g_dispatch, 0x80);
    W(menu + 0xC4, (R(menu + 0xC4) & ~0x000F000Fu) | 0x100010u);
    W(menu + 0xC8, R(menu + 0xC8) | 0x10u);
    return true;
}

// the pointer for this menu, sampled once a dispatch
uint32_t Sample(uint32_t menu, mouse::Pointer& p) {
    if (g_dispatch && g_dispatch->menu == menu && g_dispatch->sampled) {
        p = g_dispatch->pointer;
        return g_dispatch->actions;
    }
    p = mouse::MenuPointer(true);
    if ((R(menu + 0xC4) & 0x3F) && !p.wheel) g.wheel_pending = 0;
    uint32_t actions = Actions(g, menu, p);
    if (g_dispatch && g_dispatch->menu == menu) {
        g_dispatch->pointer = p;
        g_dispatch->actions = actions;
        g_dispatch->sampled = true;
    }
    return actions;
}

// after the pad is read, before the footer's shortcuts are acted on: a click on a prompt or a tab
void FooterInput(X86Context& ctx, uint32_t menu) {
    Dispatch* d = g_dispatch;
    if (!d || d->sampled || d->menu != menu || !MenuContext() || !LiveMenu(menu) ||
        !kernel::CallGuest(ctx, kMenuActive, {menu}))
        return;
    mouse::InMenu();
    mouse::Pointer p;
    uint32_t actions = Sample(menu, p);
    // a text page scrolls by held up or down in the input tick itself: one notch, one tick
    if ((actions & (kWheelUp | kWheelDown)) && (R(menu + 0x40) & 8))
        W(menu + 0xC8, (R(menu + 0xC8) & ~0xFu) | ((actions & kWheelUp) ? 1u : 2u));
    float x, y;
    if (!(actions & kClick) || !CanvasPoint(p, x, y)) return;
    if (uint32_t tab = TabHit(ctx, menu, x, y)) {
        d->tab_menu = menu;
        d->tab_target = tab;
        d->actions = 0;
        return;
    }
    int button = ProfileHit(ctx, menu, x, y);
    if (button < 0) button = FooterHit(ctx, menu, x, y);
    if (button >= 0) {
        static const uint32_t kRaw[] = {0x80, 0x120, 0x10, 0x40};  // A, B and Back, X, Y on the pad
        uint32_t action = 0x10u << button;
        PressPad(*d, kRaw[button]);
        W(menu + 0xC4, (R(menu + 0xC4) & ~0x000F000Fu) | action | (action << 16));
        W(menu + 0xC8, R(menu + 0xC8) | action);
        d->actions = 0;  // a prompt's click never also picks a row
        g.owns = true;
    }
}

void Pointing(X86Context& ctx, uint32_t menu) {
    if (!MenuContext() || !LiveMenu(menu) || !kernel::CallGuest(ctx, kMenuActive, {menu})) {
        if (g_drag_owner.menu == menu) g_drag = -1;
        return;
    }
    mouse::InMenu();
    if (KeyboardMenu(menu)) {
        mouse::SetTextEntry(true);
        if (Typing(ctx, menu)) return;  // the typed key, not the row under the pointer
    }
    // MapMaker's cursor on screen: the clicks are its A and B, not the menu's too
    if (!MenuScreen() && mouse::GameCursorMode()) return;
    mouse::Pointer p;
    uint32_t actions = Sample(menu, p);
    float x, y;
    if (!CanvasPoint(p, x, y)) {
        g.wheel_pending = 0;
        g_drag = -1;
        return;
    }
    if (DragSlider(ctx, menu, p, actions, x, y)) return;
    uint32_t hit = RowHit(menu, x, y);
    uint32_t direction = ArrowHit(menu, x, y, hit);
    static const bool trace = std::getenv("XBE_TRACE_MENU") != nullptr;
    if (trace && (actions & ~kHover || hit != g_trace_hit)) {
        g_trace_hit = hit;
        LOGR("menu: %08X desc %08X at %.0f,%.0f actions %X row %08X id %d arrow %u selected %08X", menu, R(menu + 4), x, y,
             actions, hit, hit ? int(R(hit + 0x14)) : -1, direction, R(menu + 0x198));
    }
    // a click's first time in each menu kind, with the row it found: which menus a click does nothing in
    if (actions & kClick) {
        static uint32_t told[32], n_told = 0;
        uint32_t desc = R(menu + 4);
        if (std::find(told, told + n_told, desc) == told + n_told && n_told < 32) {
            told[n_told++] = desc;
            LOGR("menu: click in menu %08X (descriptor %08X, %s dispatch) at %.0f,%.0f: row %08X type %u id %d",
                 menu, desc, g_dispatch && g_dispatch->menu == menu ? "its" : g_dispatch ? "another" : "no", x, y, hit,
                 hit ? R(hit) : 0, hit ? int(R(hit + 0x14)) : -1);
            if (!hit) {
                uint32_t row = R(menu + 0x178);
                for (int k = 0; row && k < 24 && Valid(row, 0x10C); k++, row = R(row + 4))
                    LOGR("menu:   row type %u id %d flags %X at %.0f,%.0f-%.0f,%.0f", R(row), int(R(row + 0x14)),
                         R(row + 0x28), F(row + 0x78), F(row + 0x7C), F(row + 0x80), F(row + 0x84));
            }
        }
    }
    // the pointer owns the selection once it moves or clicks; a fresh key or pad direction takes it back
    if ((hit && (actions & (kHover | kClick))) || (actions & (kRightClick | kWheelUp | kWheelDown))) g.owns = true;
    else if (R(menu + 0xC4) & 0xF) g.owns = false;
    if (g.owns) W(menu + 0xC4, R(menu + 0xC4) & ~0x000F000Fu);
    if (actions & (kWheelUp | kWheelDown)) {
        W(menu + 0xC8, R(menu + 0xC8) & ~0xFu);
        W(menu + 0xC4, R(menu + 0xC4) | ((actions & kWheelUp) ? 0x10001u : 0x20002u));
    }
    if (hit && (actions & (kHover | kClick))) {
        if (R(menu + 0x198) != hit) {
            X86Context saved = ctx;
            kernel::CallGuest(ctx, kSelectRow, {menu, hit});
            ctx = saved;
        }
        if (actions & kClick) {
            if (direction) {
                // one step of the choice, as its arrow key would; the game wraps, skips and saves
                W(menu + 0xC4, (R(menu + 0xC4) & ~0x000F001Fu) | (direction << 16));
                W(menu + 0xC8, R(menu + 0xC8) & ~0x1Fu);
                g.wheel_pending = 0;
                return;
            }
            if (g_dispatch && g_dispatch->menu == menu && !g_dispatch->injected) PressPad(*g_dispatch, 0x80);
            W(menu + 0xC4, R(menu + 0xC4) | 0x100010u);
            W(menu + 0xC8, R(menu + 0xC8) | 0x10u);
        }
    }
    if (actions & kRightClick) {
        // the pause screen's root pages have no Back: the right button closes the pause, after the dispatch
        if (g_dispatch && g_dispatch->menu == menu && CanClosePause(menu)) g_dispatch->close_pause = menu;
        else W(menu + 0xC4, R(menu + 0xC4) | 0x20u);
    }
}

}  // namespace

// MenuInput(menu): the rows are laid out, the actions not yet taken
X86_EXTERN_FUNC(__imp__sub_00073750);
X86_FUNC(sub_00073750) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    __imp__sub_00073750(ctx, base);
    X86Context saved = ctx;
    Pointing(ctx, menu);
    ctx = saved;
}

// MenuReadPad(menu): the pad's actions gathered, the footer's shortcuts not yet acted on
X86_EXTERN_FUNC(__imp__sub_00073870);
X86_FUNC(sub_00073870) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    __imp__sub_00073870(ctx, base);
    X86Context saved = ctx;
    FooterInput(ctx, menu);
    ctx = saved;
}

// MenuDispatch(menu): takes the actions, runs the callbacks; a pause close or tab change waits for it to finish
X86_EXTERN_FUNC(__imp__sub_0007A7B0);
X86_FUNC(sub_0007A7B0) {
    Dispatch d;
    d.menu = kernel::R32(ctx.esp + 4);
    Dispatch* previous = g_dispatch;
    g_dispatch = &d;
    {
        X86Context saved = ctx;
        CarouselInput(ctx, d);
        ctx = saved;
    }
    __imp__sub_0007A7B0(ctx, base);
    UnpressPad(d);
    g_dispatch = previous;
    X86Context saved = ctx;
    if (d.close_pause && previous) previous->close_pause = d.close_pause;
    else if (d.close_pause && CanClosePause(d.close_pause)) kernel::CallGuest(ctx, kClosePause, {0});
    if (d.tab_target && previous) {
        previous->tab_menu = d.tab_menu;
        previous->tab_target = d.tab_target;
    } else if (d.tab_target && !d.close_pause) {
        ChangeTab(ctx, d.tab_menu, d.tab_target);
    }
    ctx = saved;
}

// DrawText(x, y, text, colour): the footer's prompts, the pause tabs and the profile prompts give their hit areas;
// the one under the pointer is drawn in the highlight colour
X86_EXTERN_FUNC(__imp__sub_00041DC0);
X86_FUNC(sub_00041DC0) {
    uint32_t sp = ctx.esp, colour = kernel::R32(sp + 16);
    bool hover;
    {
        X86Context saved = ctx;
        hover = NoteText(ctx);
        ctx = saved;
    }
    if (hover) W(sp + 16, kHighlight | (colour & 0xFF));
    __imp__sub_00041DC0(ctx, base);
    W(sp + 16, colour);
}

// the footer painters (menu, ...): generic and the two themed ones
static void FooterPaint(X86Context& ctx, uint8_t* base, void (*original)(X86Context&, uint8_t*)) {
    uint32_t menu = kernel::R32(ctx.esp + 4), previous = g_footer_drawing;
    if (Valid(menu, 0x23C) && R(menu + 0x38) == 0) {
        std::memset(g_footer, 0, sizeof g_footer);
        g_footer_drawing = menu;
    } else {
        g_footer_drawing = 0;
    }
    original(ctx, base);
    g_footer_drawing = previous;
}
X86_EXTERN_FUNC(__imp__sub_000765A0);
X86_FUNC(sub_000765A0) { FooterPaint(ctx, base, __imp__sub_000765A0); }
X86_EXTERN_FUNC(__imp__sub_002D91F0);
X86_FUNC(sub_002D91F0) { FooterPaint(ctx, base, __imp__sub_002D91F0); }
X86_EXTERN_FUNC(__imp__sub_002F3CA0);
X86_FUNC(sub_002F3CA0) { FooterPaint(ctx, base, __imp__sub_002F3CA0); }

// the choice arrows: right (8) and left (4), drawn with their rectangle
X86_EXTERN_FUNC(__imp__sub_00081AD0);
X86_FUNC(sub_00081AD0) {
    NoteArrow(ctx, 8);
    __imp__sub_00081AD0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00081B20);
X86_FUNC(sub_00081B20) {
    NoteArrow(ctx, 4);
    __imp__sub_00081B20(ctx, base);
}

// custom choice row painters, which use ESI for themselves: their row is the second argument
X86_EXTERN_FUNC(__imp__sub_0019E970);
X86_FUNC(sub_0019E970) {
    uint32_t previous = g_choice_drawing;
    g_choice_drawing = kernel::R32(ctx.esp + 8);
    __imp__sub_0019E970(ctx, base);
    g_choice_drawing = previous;
}
X86_EXTERN_FUNC(__imp__sub_00360710);
X86_FUNC(sub_00360710) {
    uint32_t previous = g_choice_drawing;
    g_choice_drawing = kernel::R32(ctx.esp + 8);
    __imp__sub_00360710(ctx, base);
    g_choice_drawing = previous;
}

// the audio rows' painters (menu, row, rectangle), full and compact
X86_EXTERN_FUNC(__imp__sub_002C3500);
X86_FUNC(sub_002C3500) {
    NoteAudioBars(ctx, false);
    __imp__sub_002C3500(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_002C3610);
X86_FUNC(sub_002C3610) {
    NoteAudioBars(ctx, true);
    __imp__sub_002C3610(ctx, base);
}

// the controller sensitivity row's painter (menu, row), and the bar painter it calls (x, y, w, h as integers)
X86_EXTERN_FUNC(__imp__sub_002EA250);
X86_FUNC(sub_002EA250) {
    uint32_t menu = g_sensitivity_menu, row = g_sensitivity_row;
    g_sensitivity_menu = kernel::R32(ctx.esp + 4);
    g_sensitivity_row = kernel::R32(ctx.esp + 8);
    g_sliders[kStickSensitivity] = {};
    __imp__sub_002EA250(ctx, base);
    g_sensitivity_menu = menu;
    g_sensitivity_row = row;
}
X86_EXTERN_FUNC(__imp__sub_000814B0);
X86_FUNC(sub_000814B0) {
    uint32_t sp = ctx.esp;
    if (g_sensitivity_menu && R(sp) == 0x2EA3E0 && Valid(g_sensitivity_row, 0x10C) &&
        R(g_sensitivity_row + 0x14) == 9 && R(kMenuPlayer) < 4)
        NoteSlider(kStickSensitivity, g_sensitivity_menu, g_sensitivity_row, float(int32_t(R(sp + 8))),
                   float(int32_t(R(sp + 12))), float(int32_t(R(sp + 16))), float(int32_t(R(sp + 20))));
    __imp__sub_000814B0(ctx, base);
}

// the character carousel's painter (menu) and the quads it draws its portraits with; its team icons TimeSplitters 2's
// in the early style
namespace game {
void WithTs2Teams(X86Context& ctx, uint8_t* base, void (*draw)(X86Context&, uint8_t*));  // hud_style.cpp
}
X86_EXTERN_FUNC(__imp__sub_002C4D20);
X86_FUNC(sub_002C4D20) {
    uint32_t previous = g_carousel_drawing, menu = kernel::R32(ctx.esp + 4);
    g_carousel_drawing = CarouselActive(menu) ? menu : 0;
    if (g_carousel_drawing) g_portrait_count = 0;
    game::WithTs2Teams(ctx, base, __imp__sub_002C4D20);
    g_carousel_drawing = previous;
}
bool NameTagMarker(uint32_t ret);  // name_tags.cpp
namespace game {
bool EarlyTeamMarker(X86Context& ctx);  // hud_style.cpp
bool EarlyRankBox(X86Context& ctx);     // hud_style.cpp
}
X86_EXTERN_FUNC(__imp__sub_0001BA70);
X86_FUNC(sub_0001BA70) {
    if (NameTagMarker(kernel::R32(ctx.esp)) || game::EarlyTeamMarker(ctx) || game::EarlyRankBox(ctx)) {
        kernel::Return(ctx, 0, 0);
        return;
    }
    NoteQuad(ctx);
    __imp__sub_0001BA70(ctx, base);
}

// the team chooser: its painter, called with the pause's transition at +0x25C, and its parent's tick (menu), which
// reads the pad's newly pressed buttons before the child menu: a click on an arrow or prompt is a pad press
X86_EXTERN_FUNC(__imp__sub_002E4E90);
X86_FUNC(sub_002E4E90) {
    uint32_t previous = g_team_drawing, pause = R(kPause);
    g_team_drawing = 0;
    if (R(ctx.esp) == 0x2EE804 && Valid(pause, 0x268) && R(ctx.esp + 4) == pause + 0x25C) {
        uint32_t root = R(kPauseRoot);
        if (Valid(root, 0x23C)) g_team_drawing = TeamChild(R(root + 0x170));
    }
    __imp__sub_002E4E90(ctx, base);
    g_team_drawing = previous;
}
X86_EXTERN_FUNC(__imp__sub_002EDDC0);
X86_FUNC(sub_002EDDC0) {
    uint32_t child = TeamChild(kernel::R32(ctx.esp + 4)), player = TeamPlayer();
    Dispatch edge;
    if (child && player && MenuContext()) {
        mouse::InMenu();
        mouse::Pointer p = mouse::MenuPointer(true);
        uint32_t actions = Actions(g, child, p);
        float x, y;
        if ((actions & kClick) && CanvasPoint(p, x, y)) {
            int footer = FooterHit(ctx, child, x, y);
            if (footer == 0 || footer == 1) {
                PressPad(edge, footer ? 0x120 : 0x80);
            } else if (!R(player + 0x11C)) {
                for (int i = 0; i < 2; i++) {
                    const TeamArrow& a = g_team_arrows[i];
                    if (a.menu == child && a.player == player && a.team == R(player + 0x48) && NowMs() - a.time <= 100 &&
                        x >= a.left && x < a.right && y >= a.top && y < a.bottom) {
                        PressPad(edge, i ? 2 : 1);  // the D-pad's right or left
                        break;
                    }
                }
            }
        }
    }
    __imp__sub_002EDDC0(ctx, base);
    UnpressPad(edge);
}
