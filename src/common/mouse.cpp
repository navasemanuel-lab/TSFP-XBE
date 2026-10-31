// The mouse: raw counts for aiming, untouched by the Windows pointer speed and acceleration; the cursor locked in the
// window, hidden in play and an arrow in the menus. The settings panel (F1, panel.cpp) frees it while it is open, Alt
// held frees it for a moment.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

#include "app/app.h"
#include "common/mouse.h"
#include "common/options.h"
#include "core/log.h"
#include "net/lockstep.h"

namespace mouse {

namespace {

HWND g_hwnd = nullptr;
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<int> g_wheel{0};         // menu notches
std::atomic<int> g_play_notches{0};  // play notches not yet pressed
std::atomic<bool> g_unlocked{false};  // the settings panel is open
std::atomic<bool> g_block_buttons{false};  // until both buttons are up
std::atomic<uint64_t> g_aim_ms{0};   // the last look update
std::atomic<uint64_t> g_menu_ms{0};  // the last tick of a menu with the pointer
std::atomic<uint64_t> g_game_cursor_ms{0};  // the last frame the game drew its own cursor
constexpr uint64_t kRecentMs = 250;  // updates this recent: in play, or in a menu
constexpr uint64_t kNotchMs = 50;    // a notch's press, and the gap after it
std::mutex g_test_lock;
Pointer g_test;
uint64_t g_test_ms = 0;
constexpr uint64_t kTestMs = 100;
std::atomic<uint64_t> g_text_ms{0};  // the last tick of an on-screen keyboard
std::mutex g_chars_lock;
std::string g_chars;  // typed, not yet taken

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// a scripted run (XBE_INPUT_SCRIPT) takes no mouse, as controls.cpp no keyboard
bool Foreground() {
    static const bool scripted = std::getenv("XBE_INPUT_SCRIPT") != nullptr;
    return !scripted && g_hwnd && GetForegroundWindow() == g_hwnd;
}
bool Play() { return NowMs() - g_aim_ms < kRecentMs; }
bool Down(int vk) { return GetAsyncKeyState(vk) < 0; }
bool AltHeld() { return Down(VK_MENU); }
bool Locked() { return Foreground() && !g_unlocked && !AltHeld() && !IsIconic(g_hwnd); }

// window thread: the cursor held in the client area while locked
void UpdateClip() {
    RECT cur;
    if (!Locked()) {
        if (GetClipCursor(&cur)) {
            RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            if (!EqualRect(&cur, &screen)) ClipCursor(nullptr);
        }
        return;
    }
    RECT r;
    GetClientRect(g_hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (!GetClipCursor(&cur) || !EqualRect(&r, &cur)) ClipCursor(&r);
}

// Windows asks for the cursor only when the mouse moves
void RefreshCursor() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void SetLocked(bool on) {
    if (g_unlocked.exchange(!on) == !on) return;
    LOGR("mouse: %s", on ? "locked in the window" : "free for the settings panel");
    g_dx = g_dy = 0;
    UpdateClip();
    RefreshCursor();
}

void OnRawInput(void* handle) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(static_cast<HRAWINPUT>(handle), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE || !Foreground()) return;
    UpdateClip();
    const RAWMOUSE& m = ri.data.mouse;
    int notch = (m.usButtonFlags & RI_MOUSE_WHEEL) ? (SHORT(m.usButtonData) > 0 ? 1 : -1) : 0;
    if (!Play()) {
        g_wheel += notch;
        return;
    }
    if (!Locked()) return;
    if (notch) g_play_notches = std::clamp(g_play_notches + notch, -4, 4);
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_dx += m.lLastX;
        g_dy += m.lLastY;
    }
}

}  // namespace

void Start(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RAWINPUTDEVICE dev{1, 2, 0, g_hwnd};  // generic desktop page, mouse usage
    RegisterRawInputDevices(&dev, 1, sizeof(dev));
}

bool Message(unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case WM_INPUT:
            OnRawInput(reinterpret_cast<void*>(lp));
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_MENU) {
                UpdateClip();
                RefreshCursor();
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp == VK_MENU) {
                UpdateClip();
                RefreshCursor();
            }
            break;
        case WM_MOUSEACTIVATE:
            g_block_buttons = true;  // the click that brings the window back
            break;
        case WM_CHAR:
            // the game's screens are 8-bit: Latin-1 is kept as is, control characters as Backspace and Enter
            if (!g_unlocked && TextEntry() && (wp == '\b' || wp == '\r' || (wp >= 0x20 && wp <= 0xFF && wp != 0x7F))) {
                std::lock_guard lock(g_chars_lock);
                if (g_chars.size() < 64) g_chars += char(wp);
            }
            break;
        case WM_SYSCOMMAND:
            // Alt frees the cursor; alone it must not open the window menu
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                *result = 0;
                return true;
            }
            break;
        case WM_KILLFOCUS:
            ClipCursor(nullptr);
            break;
        case WM_ACTIVATE:
        case WM_SIZE:
        case WM_MOVE:
            UpdateClip();
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) != HTCLIENT || !Foreground()) break;
            if (Locked() && (Play() || GameCursorMode())) ::SetCursor(nullptr);
            else ::SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
            *result = TRUE;
            return true;
    }
    return false;
}

void KeepLocked() {
    // Windows and other programs (overlays, remote desktops) drop the clip on their own; it is put back every frame,
    // and in play the hidden cursor is kept in the middle so it never reaches an edge
    // the cursor shown or hidden at once when play starts or stops (a pause), not on the next move
    static bool was_playing = false;
    if (bool playing = Play() || GameCursorMode(); playing != was_playing) {
        was_playing = playing;
        RefreshCursor();
    }
    if (!g_hwnd || !Locked()) return;
    UpdateClip();
    if (!Play()) return;
    RECT r;
    if (!GetClientRect(g_hwnd, &r) || r.right <= 0 || r.bottom <= 0) return;
    POINT c{r.right / 2, r.bottom / 2}, cur;
    ClientToScreen(g_hwnd, &c);
    if (GetCursorPos(&cur) && (std::abs(cur.x - c.x) > 64 || std::abs(cur.y - c.y) > 64)) SetCursorPos(c.x, c.y);
}

void SetPanel(bool open) {
    if (!open) {
        g_block_buttons = true;  // the click that closed it is not a shot
        g_wheel = 0;
    }
    SetLocked(!open);
}

void Aiming() {
    if (!Play()) RefreshCursor();
    g_aim_ms = NowMs();
    app::g_play_ms = g_aim_ms.load();
}

bool TakeMotion(float& dx, float& dy) {
    int x = g_dx.exchange(0), y = g_dy.exchange(0);
    if (!x && !y) return false;
    dx = float(x);
    dy = float(y);
    return true;
}

void Inject(int dx, int dy) {
    g_dx += dx;
    g_dy += dy;
}

bool ButtonsBlocked() {
    if (g_block_buttons && !Down(VK_LBUTTON) && !Down(VK_RBUTTON)) g_block_buttons = false;
    return g_block_buttons || !Locked();
}

int WheelButton() {
    static uint64_t started = 0;
    static int pressing = 0;
    uint64_t now = NowMs();
    if (pressing) {
        if (now - started < kNotchMs) return pressing;
        if (now - started < 2 * kNotchMs) return 0;
        pressing = 0;
    }
    int n = g_play_notches;
    if (!n) return 0;
    int dir = n > 0 ? 1 : -1;
    g_play_notches -= dir;
    pressing = dir;
    started = now;
    return dir;
}

void InMenu() {
    if (NowMs() - g_menu_ms >= kRecentMs) RefreshCursor();
    g_menu_ms = NowMs();
    app::g_menu_ms = g_menu_ms.load();
}

bool MenuMode() { return NowMs() - g_menu_ms < kRecentMs && !Play(); }

namespace {
std::atomic<bool> g_zoomed{false};
std::atomic<uint64_t> g_terminal_ms{0};
}
void InTerminal() { g_terminal_ms = NowMs(); }

namespace {
std::atomic<uint64_t> g_vehicle_ms{0};
}
void InVehicle() { g_vehicle_ms = NowMs(); }

// The counts' speed, smoothed over a few polls, as a tilt: kFullSpeed counts a second at sensitivity 100 tilt it all
// the way. The tilt eases back to rest within kSettleMs of the mouse stopping.
bool VehicleStick(int16_t& rx, int16_t& ry) {
    static float vx = 0, vy = 0;
    static uint64_t last = 0;
    uint64_t now = NowMs();
    float dt = last ? float(now - last) / 1000.0f : 0.0f;
    last = now;
    if (now - g_vehicle_ms >= kRecentMs || !Locked()) {
        vx = vy = 0;
        return false;
    }
    float dx = 0, dy = 0;
    TakeMotion(dx, dy);
    constexpr float kSettleMs = 60.0f, kFullSpeed = 1500.0f;
    float keep = dt > 0 ? std::exp(-dt * 1000.0f / kSettleMs) : 1.0f;
    float rate = dt > 0 ? (1.0f - keep) / dt : 0.0f;
    vx = vx * keep + dx * rate;
    vy = vy * keep + dy * rate;
    const game::Options& o = game::Opt();
    float scale = float(o.mouse_sensitivity) / 100.0f / kFullSpeed;
    float x = std::clamp(vx * scale, -1.0f, 1.0f);
    float y = std::clamp(-vy * scale * float(o.mouse_vertical) / 100.0f * (o.invert_mouse ? -1.0f : 1.0f), -1.0f, 1.0f);
    if (std::abs(x) < 0.02f && std::abs(y) < 0.02f) return false;
    rx = int16_t(x * 32767.0f);
    ry = int16_t(y * 32767.0f);
    return true;
}
bool TerminalMode() { return NowMs() - g_terminal_ms < kRecentMs; }
void SetZoomed(bool on) { g_zoomed = on; }
bool Zoomed() { return g_zoomed && Play(); }

void InGameCursor() {
    if (NowMs() - g_game_cursor_ms >= kRecentMs) RefreshCursor();
    g_game_cursor_ms = NowMs();
}

// In an online co-op session (net/lockstep.h) the game sees only what goes over the network: the pointer in the menus,
// MapMaker's cursor and typed characters, which would act on this machine's game alone, are off (the keys and the pad
// work the menus and the on-screen keyboards)
bool GameCursorMode() { return NowMs() - g_game_cursor_ms < kRecentMs && !Play() && !lockstep::Active(); }
bool InPlay() { return Play(); }

Pointer MenuPointer(bool take) {
    Pointer p;
    {
        std::lock_guard lock(g_test_lock);
        if (NowMs() - g_test_ms < kTestMs) return g_test;
    }
    if (g_unlocked || lockstep::Active()) return p;  // the settings panel's; a co-op session's game takes no pointer
    if (take) p.wheel = g_wheel.exchange(0);
    POINT pt;
    RECT r;
    if (!Foreground() || !GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &r)) return p;
    // the frame is shown in the middle of the window at the console's aspect (gpu BlitFrame)
    float aspect = app::Aspect();
    float w = float(r.right), h = float(r.bottom);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    if (vw <= 0 || vh <= 0) return p;
    p.x = (float(pt.x) - (w - vw) / 2) / vw;
    p.y = (float(pt.y) - (h - vh) / 2) / vh;
    p.valid = p.x >= 0 && p.x < 1 && p.y >= 0 && p.y < 1;
    bool blocked = ButtonsBlocked();
    p.left = !blocked && Down(VK_LBUTTON);
    p.right = !blocked && Down(VK_RBUTTON);
    return p;
}

void SetTextEntry(bool on) {
    if (!on || lockstep::Active()) return;
    if (NowMs() - g_text_ms >= kRecentMs) {
        std::lock_guard lock(g_chars_lock);
        g_chars.clear();
    }
    g_text_ms = NowMs();
}

bool TextEntry() { return NowMs() - g_text_ms < kRecentMs; }

int TakeChar() {
    if (lockstep::Active()) return 0;
    std::lock_guard lock(g_chars_lock);
    if (g_chars.empty()) return 0;
    int c = static_cast<unsigned char>(g_chars.front());
    g_chars.erase(0, 1);
    return c;
}

void TypeChars(const char* s) {
    std::lock_guard lock(g_chars_lock);
    g_chars += s;
}

void TestPointer(float x, float y, bool left, bool right) {
    std::lock_guard lock(g_test_lock);
    g_test = {x, y, true, left, right, 0};
    g_test_ms = NowMs();
}

}  // namespace mouse
