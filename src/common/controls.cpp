// Controllers and the keyboard as the console pads: GameInfo::pad
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>

#include "app/app.h"
#include "common/controls.h"
#include "common/options.h"
#include "common/debug.h"
#include "common/icons.h"
#include "common/mouse.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "common/panel.h"

namespace game {
bool RightButtonFiresLeftTrigger();  // game/aim.cpp
void ToggleFreeCam();                // game/free_cam.cpp
bool FreeCamTakes(const input::Pad& pad);
}

namespace controls {

namespace {

constexpr int16_t kStickMax = 32767;
constexpr uint8_t kAnalogOn = 0xFF;
// Pad::analog
enum { kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger };
// Pad::buttons
enum : uint16_t {
    kUp = 1 << 0, kDown = 1 << 1, kLeft = 1 << 2, kRight = 1 << 3,
    kStart = 1 << 4, kBack = 1 << 5, kLeftThumb = 1 << 6, kRightThumb = 1 << 7,
};

// The pad's controls as the game uses them, after the TimeSplitters Future Perfect PC port
// (TheDoffman/timesplitters-future-perfect-pc): the left trigger throws a grenade, the right stick's click is the aim
// mode, the D-pad the weapon controls (up the weapon mode or zoom in, down the grenade type or zoom out, left and
// right, and the wheel, the previous and next weapon). Each takes the keys in the settings file's [keys] table, by
// these names, else its defaults; the first key of a button is the one its prompts show. The weapon keys press no
// button: each picks the weapon of its place in the inventory (game/weapon_keys.cpp), 1 to 9 and 0 for the tenth.
struct Action {
    const char* id;
    int analog;     // Pad::analog index, -1 none
    uint16_t bits;  // Pad::buttons
    const char* defaults;
    int weapon = 0;  // the inventory place a weapon key picks, 1 to 10
};
constexpr Action kActions[] = {
    {"action", kA, 0, "E,Enter,Space"},  {"back_melee", kB, 0, "F,Mouse4,Backspace"},
    {"reload", kX, 0, "R"},              {"y", kY, 0, "Y"},
    {"black", kBlack, 0, "X"},           {"white", kWhite, 0, "Z"},
    {"pause", -1, kStart, "Escape"},     {"objectives", -1, kBack, "Tab"},
    {"grenade", kLeftTrigger, 0, "Mouse3,Mouse5"},
    {"fire", kRightTrigger, 0, "Mouse1"},
    {"weapon_mode", -1, kUp, "Q,Up,PageUp"},
    {"grenade_type", -1, kDown, "V,Down,PageDown"},
    {"previous_weapon", -1, kLeft, "Left"},
    {"next_weapon", -1, kRight, "Right"},
    {"crouch", -1, kLeftThumb, "Ctrl,C"},
    {"aim", -1, kRightThumb, "Mouse2"},
    {"weapon_1", -1, 0, "1", 1},         {"weapon_2", -1, 0, "2", 2},
    {"weapon_3", -1, 0, "3", 3},         {"weapon_4", -1, 0, "4", 4},
    {"weapon_5", -1, 0, "5", 5},         {"weapon_6", -1, 0, "6", 6},
    {"weapon_7", -1, 0, "7", 7},         {"weapon_8", -1, 0, "8", 8},
    {"weapon_9", -1, 0, "9", 9},         {"weapon_10", -1, 0, "0", 10},
};

// key names: letters and digits as themselves, and these
struct NamedKey {
    const char* name;
    int vk;
    const char* label;  // on the button prompts
};
constexpr NamedKey kNames[] = {
    {"Mouse1", VK_LBUTTON, "LMB"}, {"Mouse2", VK_RBUTTON, "RMB"}, {"Mouse3", VK_MBUTTON, "MMB"},
    {"Mouse4", VK_XBUTTON1, "M4"}, {"Mouse5", VK_XBUTTON2, "M5"}, {"Enter", VK_RETURN, "ENTER"},
    {"Space", VK_SPACE, "SPACE"},  {"Backspace", VK_BACK, "BKSP"}, {"Escape", VK_ESCAPE, "ESC"},
    {"Tab", VK_TAB, "TAB"},        {"Ctrl", VK_CONTROL, "CTRL"},   {"Shift", VK_SHIFT, "SHIFT"},
    {"CapsLock", VK_CAPITAL, "CAPS"}, {"Up", VK_UP, "UP"},         {"Down", VK_DOWN, "DOWN"},
    {"Left", VK_LEFT, "LEFT"},     {"Right", VK_RIGHT, "RIGHT"},   {"PageUp", VK_PRIOR, "PGUP"},
    {"PageDown", VK_NEXT, "PGDN"}, {"Insert", VK_INSERT, "INS"},   {"Delete", VK_DELETE, "DEL"},
    {"Home", VK_HOME, "HOME"},     {"End", VK_END, "END"},         {"F2", VK_F2, "F2"},
    {"F3", VK_F3, "F3"},           {"F4", VK_F4, "F4"},            {"F5", VK_F5, "F5"},
    {"F6", VK_F6, "F6"},           {"F7", VK_F7, "F7"},            {"F8", VK_F8, "F8"},
    {"F9", VK_F9, "F9"},           {"F10", VK_F10, "F10"},         {"F11", VK_F11, "F11"},
    {"F12", VK_F12, "F12"},
};

// a key the keyboard and mouse press
struct Key {
    int vk;
    int analog;
    uint16_t bits;
    std::string label;
    int weapon;
};
std::vector<Key> g_keys;  // built by LoadKeys on the game's thread: before it runs, then at a poll
std::mutex g_set_lock;
std::vector<std::pair<std::string, std::string>> g_set;  // bindings the settings panel changed, not yet applied

// the weapon key pressed last, taken by the game's next weapon input; one it has not taken in a moment (a menu, a
// pause) is dropped
std::atomic<int> g_weapon{0};
std::atomic<int64_t> g_weapon_at{0};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ParseKey(std::string name, int& vk, std::string& label) {
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() == 1 && std::isalnum(uint8_t(name[0]))) {
        vk = std::toupper(uint8_t(name[0]));
        label = std::string(1, char(vk));
        return true;
    }
    for (const NamedKey& k : kNames)
        if (_stricmp(k.name, name.c_str()) == 0) {
            vk = k.vk;
            label = k.label;
            return true;
        }
    return false;
}
constexpr int kStickKeys[] = {'W', 'A', 'S', 'D'};
bool MouseButton(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

std::atomic<bool> g_keyboard{true};  // the keyboard and mouse were used last, not a controller
std::atomic<input::Family> g_family{input::Family::None};  // the controller used last

// a controller's buttons as its own markings; one SDL cannot name keeps the game's Xbox icons
icons::Style PromptStyle(input::Family f) {
    switch (f) {
        case input::Family::Xbox: return icons::Style::Xbox;
        case input::Family::PlayStation4: return icons::Style::PlayStation4;
        case input::Family::PlayStation5: return icons::Style::PlayStation5;
        case input::Family::Switch: return icons::Style::Switch;
        default: return icons::Style::Original;
    }
}

bool Down(int vk) { return GetAsyncKeyState(vk) < 0; }

// a scripted run (XBE_INPUT_SCRIPT) takes no keyboard or mouse: the PC stays usable while it plays
bool Foreground() {
    static const bool scripted = std::getenv("XBE_INPUT_SCRIPT") != nullptr;
    HWND hwnd = static_cast<HWND>(app::Window());
    return !scripted && hwnd && GetForegroundWindow() == hwnd;
}

// The right stick's aim. The game reads each axis as a byte and drops a dead zone of its own from each, apart (a
// float in each pad's state, 0x6B93D0 + 0x3C a pad), the rest stretched to full: a square dead zone, wide on a worn
// stick. Here the stick gets a small round one (Options::stick_deadzone) and a curve past it (stick_curve), and each
// axis is put past the game's dead zone by as much as the game takes off, so what it reads is what was shaped here.
constexpr uint32_t kPadStates = 0x6B93D0, kPadStateSize = 0x3C;
void ShapeLook(input::Pad& p) {
    if (!p.connected || (!p.rx && !p.ry)) return;
    const game::Options& o = game::Opt();
    float x = float(p.rx) / kStickMax, y = float(p.ry) / kStickMax;
    float r = std::hypot(x, y);
    float dz = std::min(float(o.stick_deadzone), 50.0f) / 100.0f;
    if (r <= dz) {
        p.rx = p.ry = 0;
        return;
    }
    float n = std::pow(std::min((r - dz) / (1 - dz), 1.0f), std::clamp(float(o.stick_curve), 50.0f, 300.0f) / 100.0f);
    float game_dz = 0;
    for (uint32_t i = 0; i < 4; i++) {
        float d = std::bit_cast<float>(kernel::R32(kPadStates + i * kPadStateSize));
        if (d > game_dz && d < 0.9f) game_dz = d;
    }
    LOG_FIRST(1, "controls: the game's stick dead zone %.3f", game_dz);
    auto axis = [&](float c) {
        float v = c / r * n;
        if (std::abs(v) < 1e-4f) return int16_t(0);
        return int16_t(std::copysign(std::min(game_dz + std::abs(v) * (1 - game_dz), 1.0f), v) * kStickMax);
    };
    p.rx = axis(x);
    p.ry = axis(y);
}

// A pad that has not reported yet can read its triggers half pressed (Proton, Steam Deck, some HID pads): the game
// would see fire held. A trigger counts once it has been seen released.
void TriggerRest(int port, input::Pad& pad) {
    constexpr uint8_t kReleased = 0x20;
    static std::atomic<uint8_t> rested[4];
    if (!pad.connected) {
        rested[port] = 0;
        return;
    }
    for (int t = 0; t < 2; t++) {
        uint8_t& v = pad.analog[kLeftTrigger + t];
        uint8_t bit = uint8_t(1u << t);
        if (v <= kReleased) rested[port] |= bit;
        if (!(rested[port] & bit)) v = 0;
    }
}

// a controller's own input, before the keys join it: deliberate presses, not stick drift
bool Used(const input::Pad& pad) {
    constexpr int16_t kPushed = 16000;
    auto pushed = [](int16_t v) { return v > kPushed || v < -kPushed; };
    bool analog = std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t a) { return a > 0x40; });
    return pad.connected && (pad.buttons || analog || pushed(pad.lx) || pushed(pad.ly) || pushed(pad.rx) || pushed(pad.ry));
}

// the first real input of each controller, so a log shows whether a pad reaches the game
void NoteInput(int port, const input::Pad& pad) {
    static std::atomic<bool> seen[4];
    if (seen[port] || !Used(pad)) return;
    seen[port] = true;
    LOGR("input: controller %d works", port + 1);
}

// XBE_TRACE_INPUT set: every change of a controller's report goes to the log
void TraceInput(int port, const input::Pad& pad) {
    static const bool on = std::getenv("XBE_TRACE_INPUT") != nullptr;
    static input::Pad last[4];
    if (!on || std::memcmp(&pad, &last[port], sizeof(pad)) == 0) return;
    last[port] = pad;
    LOGR("input: pad %d buttons %04X analog %02X %02X %02X %02X %02X %02X %02X %02X sticks %d %d %d %d", port + 1,
         pad.buttons, pad.analog[0], pad.analog[1], pad.analog[2], pad.analog[3], pad.analog[4], pad.analog[5],
         pad.analog[6], pad.analog[7], pad.lx, pad.ly, pad.rx, pad.ry);
}

// a grenade thrown by a mouse button, logged (a few times a run) so a stray one shows which button it was
void NoteMouseGrenade(const char* label) {
    static auto last = std::chrono::steady_clock::time_point{};
    static int count = 0;
    auto now = std::chrono::steady_clock::now();
    if (count >= 20 || now - last < std::chrono::seconds(1)) return;
    last = now;
    count++;
    LOGR("input: grenade from %s", label);
}

// the keyboard joins port 1's pad: WASD the left stick, the arrows the right one; true when a key was held
bool Keyboard(input::Pad& p) {
    if (!Foreground()) return false;
    auto stick = [](int neg, int pos) { return int16_t((Down(pos) ? kStickMax : 0) - (Down(neg) ? kStickMax : 0)); };
    bool typing = mouse::TextEntry();
    if (!typing) {
        if (int16_t x = stick('A', 'D')) p.lx = x;
        if (int16_t y = stick('S', 'W')) p.ly = y;
    }
    bool any = !typing && std::any_of(std::begin(kStickKeys), std::end(kStickKeys), Down);
    // a menu's pointer has the left and right buttons, a free cursor all of them; one held into or out of a menu stays
    // with what it was pressed for
    static bool held_from_menu = false;
    bool menu = mouse::MenuMode(), buttons = Down(VK_LBUTTON) || Down(VK_RBUTTON);
    if (menu) held_from_menu = buttons;
    else if (!buttons) held_from_menu = false;
    bool mouse_blocked = mouse::ButtonsBlocked();
    // on an on-screen keyboard the keys type: only Escape, Tab, the arrows and the mouse stay buttons
    auto types = [](int vk) {
        return vk != VK_ESCAPE && vk != VK_TAB && vk != VK_UP && vk != VK_DOWN && vk != VK_LEFT && vk != VK_RIGHT &&
               !MouseButton(vk);
    };
    // where the game draws a cursor of its own (MapMaker, a terminal), the mouse moves it and its buttons are A and B;
    // a button held across the change keeps to what it was pressed for until released
    static bool cursor_was = false, cursor_held = false;
    bool game_cursor = mouse::GameCursorMode() || mouse::TerminalMode();
    if (game_cursor != cursor_was) cursor_held = buttons;
    else if (!buttons) cursor_held = false;
    cursor_was = game_cursor;
    if (game_cursor && !mouse_blocked && !cursor_held) {
        if (Down(VK_LBUTTON)) p.analog[kA] = kAnalogOn, any = true;
        if (Down(VK_RBUTTON)) p.analog[kB] = kAnalogOn, any = true;
    }
    // the wheel pressed as it turns (a notch's push) is no grenade: the middle button counts once the wheel has been
    // still a moment
    static auto wheel_at = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    int wheel = mouse::WheelButton();
    if (wheel) wheel_at = now;
    bool wheel_turning = now - wheel_at < std::chrono::milliseconds(400);
    // in the Goliath the right button fires its minigun, the left trigger's, instead of aiming
    bool right_fires = game::RightButtonFiresLeftTrigger();
    static bool weapon_held[11] = {};
    bool weapon_down[11] = {};
    for (const Key& k : g_keys) {
        if (!Down(k.vk)) continue;
        if (k.weapon) {
            if (typing || game_cursor || menu || (MouseButton(k.vk) && mouse_blocked)) continue;
            weapon_down[k.weapon] = any = true;
            if (!weapon_held[k.weapon]) {
                g_weapon = k.weapon;
                g_weapon_at = NowMs();
            }
            continue;
        }
        if (game_cursor && (k.vk == VK_LBUTTON || k.vk == VK_RBUTTON)) continue;
        if (typing && types(k.vk)) continue;
        if (MouseButton(k.vk) && mouse_blocked) continue;
        if ((k.vk == VK_LBUTTON || k.vk == VK_RBUTTON) && (menu || held_from_menu)) continue;
        if (k.vk == VK_MBUTTON && wheel_turning) continue;
        any = true;
        if (k.vk == VK_RBUTTON && right_fires) {
            p.analog[kLeftTrigger] = kAnalogOn;
            continue;
        }
        if (k.analog == kLeftTrigger && MouseButton(k.vk)) NoteMouseGrenade(k.label.c_str());
        if (k.analog >= 0) p.analog[k.analog] = kAnalogOn;
        p.buttons |= k.bits;
    }
    std::copy(std::begin(weapon_down), std::end(weapon_down), weapon_held);
    // in play the wheel steps the weapons, up the next and down the previous; aiming (right button held) or zoomed
    // in, it zooms in and out. The choice is made when a notch is taken, so a press stays one button.
    static int wheel_button = 0;
    if (wheel) {
        if (!wheel_button) {
            bool zoom = mouse::Zoomed() || (Down(VK_RBUTTON) && !mouse_blocked);
            wheel_button = zoom ? (wheel > 0 ? kUp : kDown) : (wheel > 0 ? kRight : kLeft);
        }
        p.buttons |= uint16_t(wheel_button);
        any = true;
    } else {
        wheel_button = 0;
    }
    // a vehicle or gun that turns by its stick step: the mouse tilts the right stick
    int16_t rx, ry;
    if (mouse::VehicleStick(rx, ry)) {
        p.rx = rx;
        p.ry = ry;
        any = true;
    }
    if (any) p.connected = true;
    return any;
}

// XBE_INPUT_SCRIPT="<seconds>:<button>[*<hold seconds>],...": presses for testing without hands on the game, held
// 150 ms unless given; buttons A B X Y BLACK WHITE START BACK UP DOWN LEFT RIGHT LT RT, sticks at full tilt LXP LXN LYP
// LYN RXP RXN RYP RYN (P positive: right or up), the mouse moving MXP MXN MYP MYN, seconds from the first poll
void Script(input::Pad& p) {
    struct Press {
        double at;
        std::string button;
        double hold;
    };
    static const std::vector<Press> presses = [] {
        std::vector<Press> out;
        const char* s = std::getenv("XBE_INPUT_SCRIPT");
        for (std::string rest = s ? s : ""; !rest.empty();) {
            size_t comma = rest.find(',');
            std::string item = rest.substr(0, comma);
            rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            std::string button = item.substr(colon + 1);
            size_t star = button.find('*');
            double hold = star == std::string::npos ? 0.15 : std::atof(button.c_str() + star + 1);
            out.push_back({std::atof(item.c_str()), button.substr(0, star), hold});
        }
        return out;
    }();
    if (presses.empty()) return;
    static const auto start = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    static std::vector<bool> typed(presses.size());
    for (size_t i = 0; i < presses.size(); i++) {
        const Press& pr = presses[i];
        if (t < pr.at || t > pr.at + pr.hold) continue;
        const std::string& b = pr.button;
        // T<text> types it once, as the keyboard would (no commas; ~ is Backspace, | Enter)
        if (b.size() > 1 && b[0] == 'T') {
            if (!typed[i]) {
                typed[i] = true;
                std::string s = b.substr(1);
                for (char& c : s) c = c == '~' ? '\b' : c == '|' ? '\r' : c;
                mouse::TypeChars(s.c_str());
            }
            continue;
        }
        static const std::pair<const char*, int> kAnalogNames[] = {{"A", kA},         {"B", kB},         {"X", kX},
                                                                   {"Y", kY},         {"BLACK", kBlack}, {"WHITE", kWhite},
                                                                   {"LT", kLeftTrigger}, {"RT", kRightTrigger}};
        static const std::pair<const char*, uint16_t> kBitNames[] = {{"UP", kUp},       {"DOWN", kDown}, {"LEFT", kLeft},
                                                                     {"RIGHT", kRight}, {"START", kStart}, {"BACK", kBack}};
        for (auto& [n, i] : kAnalogNames)
            if (b == n) p.analog[i] = kAnalogOn;
        for (auto& [n, bit] : kBitNames)
            if (b == n) p.buttons |= bit;
        static const std::pair<const char*, int16_t input::Pad::*> kStickNames[] = {
            {"LX", &input::Pad::lx}, {"LY", &input::Pad::ly}, {"RX", &input::Pad::rx}, {"RY", &input::Pad::ry}};
        for (auto& [n, axis] : kStickNames)
            if (b.size() == 3 && b.compare(0, 2, n) == 0) p.*axis = b[2] == 'P' ? kStickMax : int16_t(-kStickMax);
        constexpr int kCounts = 10;  // a poll
        if (b == "MXP" || b == "MXN") mouse::Inject(b[2] == 'P' ? kCounts : -kCounts, 0);
        if (b == "MYP" || b == "MYN") mouse::Inject(0, b[2] == 'P' ? -kCounts : kCounts);
        // P<x>/<y> points at that fraction of the frame, PL and PR also hold the left or right button
        if (b.size() > 1 && b[0] == 'P') {
            size_t at = b[1] == 'L' || b[1] == 'R' ? 2 : 1;
            size_t slash = b.find('/');
            if (slash != std::string::npos)
                mouse::TestPointer(float(std::atof(b.c_str() + at)), float(std::atof(b.c_str() + slash + 1)), b[1] == 'L',
                                   b[1] == 'R');
        }
        p.connected = true;
    }
}

// Gyro aiming: player 1's controller's turn as the mouse's counts, so it turns the view wherever the mouse does (on
// foot, a mounted gun, a vehicle, zoomed in less) and in online co-op goes with the mouse's turn. Only in play: in a
// menu the counts would move its pointer.
constexpr uint32_t kGamePlayers = 0x5236C4;  // the local players chosen
void GyroAim() {
    const game::Options& o = game::Opt();
    // player 1's controller: port 1's, or with the keyboard on a port of its own the first that joins it
    int port = 0;
    if (o.keyboard_own_port) {
        port = -1;
        for (int p = 1; p < 4 && port < 0 && kernel::R32(kGamePlayers) < 2; p++)
            if (input::Read(p).connected) port = p;
    }
    float yaw, pitch;
    if (port < 0 || !input::TakeGyro(port, yaw, pitch)) return;
    static float rest_x = 0, rest_y = 0;
    if (!o.gyro_aim || !mouse::InPlay() || mouse::MenuMode() || mouse::TextEntry() || panel::Open()) {
        rest_x = rest_y = 0;
        return;
    }
    // degrees of the view as counts: the mouse's step (0.1 degrees at sensitivity 100) and up and down share undone,
    // its inversion kept
    float speed = float(std::clamp<uint32_t>(o.gyro_speed, 10, 1000)) / 100.0f;
    float count = 0.1f * float(std::max<uint32_t>(o.mouse_sensitivity, 1)) / 100.0f;
    float vertical = float(std::max<uint32_t>(o.mouse_vertical, 1)) / 100.0f;
    rest_x += -yaw * speed / count;
    rest_y += -pitch * speed / count / vertical;
    int dx = int(rest_x), dy = int(rest_y);
    rest_x -= float(dx);
    rest_y -= float(dy);
    if (dx || dy) mouse::Inject(dx, dy);
}

}  // namespace

// With the keyboard on a port of its own, a game of one player also takes the first controller on port 1: the keyboard
// and mouse and the controller work together, whichever is touched. With more players each controller is its own.
void MergeSoloPad(input::Pad& p) {
    if (!game::Opt().keyboard_own_port || kernel::R32(kGamePlayers) >= 2) return;
    for (int port = 1; port < 4; port++) {
        input::Pad c = input::Read(port);
        if (!c.connected) continue;
        TriggerRest(port, c);
        ShapeLook(c);
        if (Used(c)) {
            g_keyboard = false;
            g_family = input::PadFamily(port);
        }
        p.buttons |= c.buttons;
        for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], c.analog[i]);
        auto take = [](int16_t& mine, int16_t theirs) {
            if (std::abs(int(theirs)) > std::abs(int(mine))) mine = theirs;
        };
        take(p.lx, c.lx), take(p.ly, c.ly), take(p.rx, c.rx), take(p.ry, c.ry);
        return;
    }
}

void Pad(int port, input::Pad& pad) {
    if (port < 0 || port > 3) return;
    TraceInput(port, pad);
    TriggerRest(port, pad);
    ShapeLook(pad);
    NoteInput(port, pad);
    // the device used last decides the prompts: a controller or the keyboard and mouse, both work at once
    if (Used(pad)) {
        g_keyboard = false;
        g_family = input::PadFamily(port);
    }
    // port 1 is always there for the keyboard
    // Back and Start together open or close the settings panel; the game sees idle pads while it is open
    static bool combo_was[4] = {};
    bool combo = (pad.buttons & (kBack | kStart)) == (kBack | kStart);
    if (combo && !combo_was[port]) panel::Toggle();
    combo_was[port] = combo;
    if (port == 0) {
        pad.connected = true;
        if (std::vector<std::pair<std::string, std::string>> set; [&] {
                std::lock_guard lock(g_set_lock);
                set.swap(g_set);
                return !set.empty();
            }()) {
            for (auto& [id, keys] : set) game::Opt().keys[id] = keys;
            LoadKeys();
            icons::KeysChanged();
            app::Save();
        }
        MergeSoloPad(pad);
        if (Keyboard(pad)) g_keyboard = true;
        GyroAim();
        Script(pad);
        mouse::KeepLocked();
        debug::Tick();
        // XBE_PROMPTS=xbox|ps4|ps5|switch: those prompts whatever is used, for testing without the controller
        static const icons::Style forced = [] {
            const char* e = std::getenv("XBE_PROMPTS");
            std::string s = e ? e : "";
            return s == "xbox" ? icons::Style::Xbox : s == "ps4" ? icons::Style::PlayStation4
                 : s == "ps5" ? icons::Style::PlayStation5 : s == "switch" ? icons::Style::Switch
                 : icons::Style::Original;
        }();
        if (forced != icons::Style::Original) icons::Update(forced);
        else icons::Update(g_keyboard ? icons::Style::Keyboard : PromptStyle(g_family));
    }
    if (panel::Holds(port, pad)) {
        pad = input::Pad{.connected = pad.connected};
        if (port == 0) g_weapon = 0;
    }
    // F6, or Back with the right stick clicked: the free camera on or off; while it flies it has port 1's controls
    if (port == 0) {
        static bool cam_was = false;
        bool cam = (Foreground() && Down(VK_F6)) || (pad.buttons & (kBack | kRightThumb)) == (kBack | kRightThumb);
        if (cam && !cam_was) game::ToggleFreeCam();
        cam_was = cam;
        if (game::FreeCamTakes(pad)) {
            pad = input::Pad{.connected = pad.connected};
            g_weapon = 0;
        }
    }
}

void LoadKeys() {
    auto& keys = game::Opt().keys;
    g_keys.clear();
    // settings from before the weapon keys had White on 1 and Black on 2: unchanged, they move to Z and X
    if (!keys.count("weapon_1")) {
        auto move = [&](const char* id, const char* was, const char* now) {
            auto it = keys.find(id);
            if (it != keys.end() && it->second == was) it->second = now;
        };
        move("white", "1", "Z");
        move("black", "2", "X");
    }
    for (const Action& a : kActions) {
        auto parse = [&](const std::string& list, std::vector<Key>& out) {
            for (size_t start = 0; start <= list.size();) {
                size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                int vk;
                std::string label;
                if (ParseKey(list.substr(start, comma - start), vk, label))
                    out.push_back({vk, a.analog, a.bits, label, a.weapon});
                start = comma + 1;
            }
        };
        std::vector<Key> mine;
        auto it = keys.find(a.id);
        if (it != keys.end()) parse(it->second, mine);
        // unknown names leave the action its defaults; an empty list unbinds it
        if (it == keys.end() || (mine.empty() && !it->second.empty())) {
            if (it != keys.end()) LOGR("controls: no key known in %s = '%s', the defaults kept", a.id, it->second.c_str());
            mine.clear();
            parse(a.defaults, mine);
            keys[a.id] = a.defaults;
        }
        g_keys.insert(g_keys.end(), mine.begin(), mine.end());
    }
}

const char* ActionIds(size_t i) { return i < std::size(kActions) ? kActions[i].id : nullptr; }
const char* ActionDefaults(size_t i) { return i < std::size(kActions) ? kActions[i].defaults : nullptr; }

std::string KeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    for (const NamedKey& k : kNames)
        if (k.vk == vk) return k.name;
    return {};
}

void SetKeys(const std::string& id, const std::string& keys) {
    std::lock_guard lock(g_set_lock);
    g_set.push_back({id, keys});
}

bool KeyboardActive() { return g_keyboard; }

int TakeWeapon() {
    int weapon = g_weapon.exchange(0);
    return weapon && NowMs() - g_weapon_at <= 150 ? weapon : 0;
}

const char* KeyLabel(Button b) {
    static const int kAnalog[] = {kA, kB, kX, kY, kBlack, kWhite};
    for (const Key& k : g_keys) {
        bool match = b == Button::Start  ? (k.bits & kStart) != 0
                     : b == Button::Back ? (k.bits & kBack) != 0
                                         : k.analog == kAnalog[int(b)];
        if (match) return k.label.c_str();
    }
    return "";
}

std::string PromptKeys(char code) {
    // the codes as the TimeSplitters FP PC port (TheDoffman/timesplitters-future-perfect-pc) reads them, by kActions
    // index: 8 grenade, 9 fire, 10 to 13 the D-pad (up, down, left, right), 14 crouch, 15 aim; the sticks by name
    std::vector<int> actions;
    switch (code) {
        case 'A': actions = {12, 13}; break;
        case 'B': actions = {10, 11}; break;
        case '9': case 'T': actions = {10, 11, 12, 13}; break;
        case 'G': case 'J': case 'K': actions = {8}; break;
        case 'I': actions = {9}; break;
        case 'L': actions = {14}; break;
        case 'M': actions = {15}; break;
        case 'N': actions = {10}; break;
        case 'O': actions = {11}; break;
        case 'D': return "[W/A/S/D]";
        case 'P': return "[A/D]";
        case 'Q': return "[W/S]";
        case 'E': case 'R': case 'S': return "[MOUSE]";
        default: return {};
    }
    std::string text = "[";
    for (int i : actions) {
        const Action& a = kActions[i];
        auto k = std::find_if(g_keys.begin(), g_keys.end(),
                              [&](const Key& k) { return k.analog == a.analog && k.bits == a.bits; });
        if (text.size() > 1) text += '/';
        text += k != g_keys.end() ? k->label : "-";
    }
    return text + "]";
}

}  // namespace controls
