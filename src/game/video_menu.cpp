// Video Options: a row after Audio Options in the Settings menu and the pause menu opens a menu of display settings,
// built with the game's own menu calls (the choice rows as its Controls menu builds them, after the TimeSplitters
// Future Perfect PC port's Display settings, TheDoffman/timesplitters-future-perfect-pc). Accept applies and saves
// them, Cancel and Back leave them as they were. The field of view rows are the player's who opened the menu: in a
// split screen each player pauses on their own part of it, and their angles are theirs alone. In System Link and
// online play the field of view rows are not there. In online co-op the menu is not: both machines play the same
// menus, and the choices are each machine's own (F1). The game's Audio/Video Options has only sound rows and
// subtitles: it is named Audio Options. The render scale and the two fields of view are bars, as the audio volumes: the
// left and right keys step them, a held click sets them, the value in the row's caption.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // cpu/dispatch.cpp: a guest address that calls host code
}

namespace game {
void ApplyWeaponFov();    // game/fov.cpp
void ApplyScreenShape();  // game/fov.cpp
// game/menu_layout.cpp: f run with the menu letters at this menu's smaller scale
void WithVideoText(X86Context& ctx, const std::function<void()>& f);
void AddUnlockRow(X86Context& ctx, uint32_t caller);  // game/unlock_menu.cpp
bool StoryOnlineRow(X86Context& ctx);                  // game/story_online.cpp
uint32_t MansionText(uint32_t id);                     // game/mansion_cut.cpp
void NoteMenuBar(uint32_t menu, uint32_t row, float x, float y, float w, float h,
                 void (*set)(uint32_t id, float at));  // game/menu_mouse.cpp
}  // namespace game

namespace {

// the game's menu calls
constexpr uint32_t kShowMenu = 0x79B70;    // (parent, descriptor): the parent's child becomes a new menu
constexpr uint32_t kMenuTitle = 0x75B70;   // (flags, title, width percent, 0, back callback)
constexpr uint32_t kMenuLayout = 0x71B10;  // (1, padding, 1, 1)
constexpr uint32_t kMenuStyle = 0x71C50;   // (flags)
constexpr uint32_t kAddRow = 0x79D60;      // (text, id, flags, extra height, select callback, draw callback): the row
constexpr uint32_t kChoices = 0x7D480;     // (flags, 0): the last row picks one of its choices, 0x8400 values right
constexpr uint32_t kAddChoice = 0x717D0;   // (text)
constexpr uint32_t kSetChoice = 0x75430;   // (row, index)
constexpr uint32_t kFinishMenu = 0x771D0;  // (menu): the rows laid out
constexpr uint32_t kText = 0x3D7B0;        // (id): the language's text
constexpr uint32_t kChosen = 0x18C;        // a menu's chosen row id
constexpr uint32_t kChoice = 0xC8;         // a choice row's choice
constexpr uint32_t kAcceptText = 0x1B08, kCancelText = 0x1B0B;
constexpr uint32_t kFade = 0x154FE0;       // (colour, the menu's fade +0xF4): the colour as the menu is drawn now
constexpr uint32_t kDrawBar = 0x814B0;     // (fill 0-1, x, y, w, h, colour, colour, fill colour, 0): the sensitivity bar
constexpr uint32_t kSelected = 0x198;      // a menu's selected row
constexpr uint32_t kActions = 0xC4;        // a menu's actions this tick, the high word repeating: 4 left, 8 right
// the Settings menu and the pause menu add their Audio/Video row from these, with these callbacks
constexpr uint32_t kFromSettings = 0x2CC02F, kSettingsAudio = 0x2CBE10;
constexpr uint32_t kFromPause = 0x2F5666, kPauseAudio = 0x2EC1F0;
constexpr uint32_t kAudioVideoTexts[] = {0x182E, 0x182F, 0x1830};
constexpr uint32_t kPlayers = 0x790950;

enum Row : uint32_t { kMode, kResolution, kRender, kAspect, kFov, kWeaponFov, kVsync, kFiltering, kMsaa, kAntialias, kNames, kRows };
constexpr uint32_t kAcceptId = kRows, kCancelId = kRows + 1;

// the rows set by a bar: the range and the game's own value
struct Bar {
    uint32_t id, low, high;
    const char* label;
    const char* unit;
};
constexpr Bar kBars[] = {{kRender, 1, 200, "Render Scale", "%"},
                         {kFov, 40, 100, "Field of View", ""},
                         {kWeaponFov, 30, 70, "Weapon Field of View", ""}};
constexpr uint32_t kLabelSize = 40;
// common sizes from 640x480 to 7680x4320 at 4:3, 5:4, 16:9, 16:10, 21:9 and 32:9, after the PC port's catalog; beyond
// the monitor's own modes a size sets the picture's resolution, the screen keeps its mode
constexpr std::pair<uint32_t, uint32_t> kPresets[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 960},  {1600, 1200}, {1920, 1440}, {2048, 1536},
    {2880, 2160}, {1280, 1024}, {2560, 2048}, {854, 480},   {960, 540},   {1280, 720},  {1366, 768},  {1600, 900},
    {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {5120, 2880}, {7680, 4320}, {1280, 800},  {1440, 900},
    {1680, 1050}, {1920, 1200}, {2560, 1600}, {3840, 2400}, {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160},
    {3840, 1080}, {5120, 1440}, {7680, 2160}};
constexpr const char* kAspectNames[] = {"Auto", "4:3", "16:9", "16:10", "21:9", "32:9"};
constexpr uint32_t kFilterings[] = {0, 2, 4, 8, 16};

// guest memory: the descriptor and the texts that outlive a menu, then the texts of one build
constexpr uint32_t kFixedSize = 0x100, kPoolSize = 0x2000;
uint32_t g_storage = 0, g_fixed = 0, g_pool = 0, g_pool_used = 0;
uint32_t g_open_cb = 0, g_build_cb = 0, g_select_cb = 0, g_back_cb = 0, g_tick_cb = 0, g_bar_cb = 0;
uint32_t g_row_text = 0, g_audio_title = 0, g_audio_short = 0;
uint32_t g_parent = 0, g_previous = 0;
uint32_t g_slot = 0;          // the player whose angles the menu sets, 0 to 3
bool g_split = false;         // opened from a split screen's pause: the title names the player

struct Choices {
    std::vector<std::string> texts;
    std::vector<uint32_t> values;
    uint32_t current = 0;
    uint32_t row = 0;  // the guest row while the menu is built
};
Choices g_rows[kRows];
uint32_t g_value[kRows];  // a bar row's value while the menu is open
uint32_t g_label[kRows];  // a bar row's caption, rewritten as it moves
std::vector<std::pair<uint32_t, uint32_t>> g_modes;  // index 0 the desktop's

void VideoOpen(X86Context& ctx, uint8_t*);
void VideoBuild(X86Context& ctx, uint8_t*);
void VideoSelect(X86Context& ctx, uint8_t*);
void VideoBack(X86Context& ctx, uint8_t*);
void VideoTick(X86Context& ctx, uint8_t*);
void BarDraw(X86Context& ctx, uint8_t*);

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

uint32_t Copy(uint32_t va, const char* s) {
    std::memcpy(kernel::HostPtr(va), s, std::strlen(s) + 1);
    return va;
}

uint32_t Fixed(const char* s) {
    static uint32_t used = 64;  // after the descriptor, its fields past the two callbacks 0
    uint32_t n = uint32_t(std::strlen(s) + 1);
    if (used + n > kFixedSize - kLabelSize * std::size(kBars)) return g_storage + 15;  // a 0 of the descriptor's: empty
    uint32_t va = g_fixed + used;
    used += n;
    return Copy(va, s);
}

uint32_t Text(const std::string& s) {
    uint32_t n = uint32_t(s.size() + 1);
    if (g_pool_used + n > kPoolSize) return g_storage + 15;
    uint32_t va = g_pool + g_pool_used;
    g_pool_used += n;
    return Copy(va, s.c_str());
}

bool Prepare() {
    if (g_storage) return true;
    g_storage = mem::AllocSystem(kFixedSize + kPoolSize);
    if (!g_storage) return false;
    std::memset(kernel::HostPtr(g_storage), 0, kFixedSize + kPoolSize);
    g_fixed = g_storage;
    g_pool = g_storage + kFixedSize;
    g_open_cb = dispatch::HostThunk(VideoOpen);
    g_build_cb = dispatch::HostThunk(VideoBuild);
    g_select_cb = dispatch::HostThunk(VideoSelect);
    g_back_cb = dispatch::HostThunk(VideoBack);
    g_tick_cb = dispatch::HostThunk(VideoTick);
    g_bar_cb = dispatch::HostThunk(BarDraw);
    kernel::W32(g_storage, g_build_cb);
    kernel::W32(g_storage + 4, g_tick_cb);
    for (const Bar& b : kBars) {
        g_label[b.id] = g_fixed + kFixedSize - kLabelSize * (1 + uint32_t(&b - kBars));
    }
    g_row_text = Fixed("Video Options");
    g_audio_title = Fixed("Audio Options");
    g_audio_short = Fixed("Audio");
    return true;
}

// the value's place among the choices, the value added in order when the settings file has another
uint32_t Place(Choices& c, uint32_t value, const std::string& text) {
    auto it = std::find(c.values.begin(), c.values.end(), value);
    if (it != c.values.end()) return uint32_t(it - c.values.begin());
    auto at = std::upper_bound(c.values.begin(), c.values.end(), value);
    size_t i = at - c.values.begin();
    c.values.insert(at, value);
    c.texts.insert(c.texts.begin() + i, text);
    return uint32_t(i);
}

std::string Number(uint32_t v, uint32_t game_default) {
    std::string s = std::to_string(v);
    return v == game_default ? s + " (Default)" : s;
}

const Bar* BarOf(uint32_t id) {
    for (const Bar& b : kBars)
        if (b.id == id) return &b;
    return nullptr;
}

// a bar's value set, its caption with it
void SetBar(uint32_t id, uint32_t v) {
    const Bar* b = BarOf(id);
    if (!b || !g_storage) return;
    g_value[id] = std::clamp(v, b->low, b->high);
    char text[kLabelSize];
    std::snprintf(text, sizeof(text), "%s: %u%s", b->label, g_value[id], b->unit);
    Copy(g_label[id], text);
}

// a held click at this part of a bar's length
void BarPointer(uint32_t id, float at) {
    if (const Bar* b = BarOf(id)) SetBar(id, b->low + uint32_t(at * float(b->high - b->low) + 0.5f));
}

// a size's shape by the name it goes by
std::string Shape(uint32_t w, uint32_t h) {
    uint32_t a = w, b = h;
    while (b) {
        a %= b;
        std::swap(a, b);
    }
    uint32_t x = w / a, y = h / a;
    if (x == 683 && y == 384) return "16:9";  // 1366x768
    if ((x == 64 && y == 27) || (x == 43 && y == 18) || (x == 12 && y == 5)) return "21:9";
    return std::to_string(x) + ":" + std::to_string(y);
}

std::string ModeText(uint32_t w, uint32_t h) {
    return w ? std::to_string(w) + "x" + std::to_string(h) + " (" + Shape(w, h) + ")" : "Desktop";
}

// the desktop's, then the monitor's modes and the common sizes, largest first
std::vector<std::pair<uint32_t, uint32_t>> ListModes() {
    std::vector<std::pair<uint32_t, uint32_t>> modes(1, {0, 0});
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    std::vector<std::pair<uint32_t, uint32_t>> found(std::begin(kPresets), std::end(kPresets));
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi))
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
            if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
                found.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) {
        return uint64_t(a.first) * a.second > uint64_t(b.first) * b.second ||
               (uint64_t(a.first) * a.second == uint64_t(b.first) * b.second && a.first > b.first);
    });
    found.erase(std::unique(found.begin(), found.end()), found.end());
    modes.insert(modes.end(), found.begin(), found.end());
    return modes;
}

// the choices, from the settings now
void Stage() {
    const app::Settings& s = app::Get();
    const game::Options& o = game::Opt();
    for (Choices& c : g_rows) c = Choices{};

    g_rows[kMode].texts = {"Fullscreen", "Borderless", "Windowed"};
    g_rows[kMode].current = std::min<uint32_t>(uint32_t(s.display_mode), 2);

    g_modes = ListModes();
    Choices& res = g_rows[kResolution];
    for (auto [w, h] : g_modes) res.texts.push_back(ModeText(w, h));
    if (s.width && s.height) {
        auto it = std::find(g_modes.begin(), g_modes.end(), std::make_pair(s.width, s.height));
        if (it == g_modes.end()) {
            g_modes.push_back({s.width, s.height});
            res.texts.push_back(ModeText(s.width, s.height));
            it = g_modes.end() - 1;
        }
        res.current = uint32_t(it - g_modes.begin());
    }

    SetBar(kRender, s.render_percent);

    g_rows[kAspect].texts.assign(std::begin(kAspectNames), std::end(kAspectNames));
    g_rows[kAspect].current = std::min<uint32_t>(s.aspect, uint32_t(std::size(kAspectNames)) - 1);

    SetBar(kFov, game::Fov(g_slot));
    SetBar(kWeaponFov, game::WeaponFov(g_slot));

    g_rows[kVsync].texts = {"Off", "On"};
    g_rows[kVsync].current = s.vsync ? 1 : 0;

    Choices& filter = g_rows[kFiltering];
    for (uint32_t f : kFilterings) {
        filter.values.push_back(f);
        filter.texts.push_back(f ? std::to_string(f) + "x Anisotropic" : "Default");
    }
    filter.current = Place(filter, s.aniso, std::to_string(s.aniso) + "x Anisotropic");

    Choices& ms = g_rows[kMsaa];
    ms.values = {0, 2, 4, 8};
    ms.texts = {"Off", "2x", "4x", "8x"};
    ms.current = Place(ms, s.msaa, std::to_string(s.msaa) + "x");

    Choices& aa = g_rows[kAntialias];
    aa.values = {0, 1, 2};
    aa.texts = {"Off", "FXAA", "SMAA"};
    aa.current = s.edge_aa <= 2 ? s.edge_aa : 0;

    g_rows[kNames].texts = {"Off", "On"};
    g_rows[kNames].current = o.player_names ? 1 : 0;
}

void Leave(X86Context& ctx) { kernel::CallGuest(ctx, kShowMenu, {g_parent, g_previous}); }

// the choices made, into the settings and the settings file
void Accept() {
    uint32_t pick[kRows];
    for (uint32_t i = 0; i < kRows; i++) {
        Choices& c = g_rows[i];
        uint32_t n = uint32_t(c.texts.size());
        uint32_t v = c.row && Valid(c.row, kChoice + 4) ? kernel::R32(c.row + kChoice) : c.current;
        pick[i] = v < n ? v : c.current;
    }
    app::Settings& s = app::Get();
    game::Options& o = game::Opt();
    app::DisplayMode mode = app::DisplayMode(pick[kMode]);
    auto [w, h] = g_modes[pick[kResolution]];
    bool display = mode != s.display_mode || w != s.width || h != s.height || pick[kAspect] != s.aspect;
    s.display_mode = mode;
    s.width = w;
    s.height = h;
    s.render_percent = g_value[kRender];
    s.aspect = pick[kAspect];
    s.vsync = pick[kVsync] == 1;
    s.aniso = g_rows[kFiltering].values[pick[kFiltering]];
    s.msaa = g_rows[kMsaa].values[pick[kMsaa]];
    s.edge_aa = g_rows[kAntialias].values[pick[kAntialias]];
    // locked online the rows are not there: the player's own angles stay for later
    if (!game::FovLocked()) {
        game::Fov(g_slot) = g_value[kFov];
        game::WeaponFov(g_slot) = g_value[kWeaponFov];
    }
    o.player_names = pick[kNames] == 1;
    game::ApplyWeaponFov();
    game::ApplyScreenShape();
    if (display) app::ApplyDisplay();
    app::Save();
    LOGR("video: %s %ux%u, render %u%%, aspect %u, player %u fov %u, weapon fov %u, vsync %u, filtering %u, msaa %u, edges %u, profile %s, player names %u",
         mode == app::DisplayMode::Fullscreen ? "fullscreen" : mode == app::DisplayMode::Borderless ? "borderless" : "windowed",
         w, h, s.render_percent, s.aspect, g_slot + 1, game::Fov(g_slot), game::WeaponFov(g_slot), uint32_t(s.vsync), s.aniso,
         s.msaa, s.edge_aa, s.low_profile ? "low" : "high", uint32_t(o.player_names));
}

// (source menu): the video options replace it; a menu's +0x38 is its player's slot, whose part of a split screen
// it is on (the game's own rows open their menus in that slot's container, 0x2EC230)
void VideoOpen(X86Context& ctx, uint8_t*) {
    uint32_t source = kernel::R32(ctx.esp + 4);
    uint32_t slot = Valid(source, 0x3C) ? kernel::R32(source + 0x38) : 4;
    if (slot < 4 && Prepare()) {
        g_slot = slot;
        g_split = kernel::R32(kPlayers) > 1;
        g_parent = kernel::R32(source + 0x2C);
        g_previous = kernel::R32(source + 4);
        Stage();
        kernel::CallGuest(ctx, kShowMenu, {g_parent, g_storage});
    }
    kernel::Return(ctx, 0, 0);
}

// (menu): its title, a choice row per setting, Accept and Cancel, measured in the smaller letters it is drawn in
void Build(X86Context& ctx) {
    static const char* const kLabels[kRows] = {"Display Mode",   "Resolution",     "Render Scale",
                                               "Aspect Ratio",   "Field of View",  "Weapon Field of View",
                                               "Vertical Sync",  "Texture Filtering",
                                               "MSAA",           "Antialiasing",   "Player Names"};
    uint32_t menu = kernel::R32(ctx.esp + 4);
    g_pool_used = 0;
    float width = 62.0f;  // as the game's pause Options box: between the pause screen's gauges
    uint32_t width_bits;
    std::memcpy(&width_bits, &width, 4);
    uint32_t title = g_split ? Text("Video Options - Player " + std::to_string(g_slot + 1)) : g_row_text;
    kernel::CallGuest(ctx, kMenuTitle, {0xA000, title, width_bits, 0, g_back_cb});
    kernel::CallGuest(ctx, kMenuLayout, {1, 0x3F800000, 1, 1});  // 1.0f: fourteen rows fit
    kernel::CallGuest(ctx, kMenuStyle, {0x26200});
    for (uint32_t i = 0; i < kRows; i++) {
        Choices& c = g_rows[i];
        if (game::FovLocked() && (i == kFov || i == kWeaponFov)) continue;  // System Link and online: the game's
        if (BarOf(i)) {
            kernel::CallGuest(ctx, kAddRow, {g_label[i], i, 0x400, 0, 0, g_bar_cb});
            continue;
        }
        c.row = kernel::CallGuest(ctx, kAddRow, {Text(kLabels[i]), i, 0, 0, 0, 0});
        kernel::CallGuest(ctx, kChoices, {0x8400, 0});
        for (const std::string& t : c.texts) kernel::CallGuest(ctx, kAddChoice, {Text(t)});
        if (c.row) kernel::CallGuest(ctx, kSetChoice, {c.row, c.current});
    }
    uint32_t accept = kernel::CallGuest(ctx, kText, {kAcceptText}), cancel = kernel::CallGuest(ctx, kText, {kCancelText});
    kernel::CallGuest(ctx, kAddRow, {accept, kAcceptId, 0x30000, 0, g_select_cb, 0});
    kernel::CallGuest(ctx, kAddRow, {cancel, kCancelId, 0x30000, 0, g_select_cb, 0});
    kernel::CallGuest(ctx, kFinishMenu, {menu});
}

void VideoBuild(X86Context& ctx, uint8_t*) {
    game::WithVideoText(ctx, [&] { Build(ctx); });
    kernel::Return(ctx, 0, 0);
}

void Forget() {
    for (Choices& c : g_rows) c.row = 0;
}

// back to the menu it came from, the settings as they were
void VideoBack(X86Context& ctx, uint8_t*) {
    Forget();
    Leave(ctx);
    kernel::Return(ctx, 0, 0);
}

// (menu, row, rectangle): a bar row's bar, right in the row, as the PC port draws its field of view's
void BarDraw(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4), row = kernel::R32(ctx.esp + 8), rect = kernel::R32(ctx.esp + 12);
    if (Valid(menu, 0xF8) && Valid(row, 0x18) && Valid(rect, 16)) {
        uint32_t id = kernel::R32(row + 0x14);
        if (const Bar* b = BarOf(id)) {
            int32_t x = int32_t(kernel::G<float>(rect + 8) - 102.0f);
            int32_t y = int32_t((kernel::G<float>(rect + 4) + kernel::G<float>(rect + 12) - 8.0f) * 0.5f);
            uint32_t fade = kernel::R32(menu + 0xF4);
            uint32_t dark = kernel::CallGuest(ctx, kFade, {0x11193C7F, fade});
            uint32_t light = kernel::CallGuest(ctx, kFade, {0x69737F7F, fade});
            float fill = float(g_value[id] - b->low) / float(b->high - b->low);
            uint32_t fill_bits;
            std::memcpy(&fill_bits, &fill, 4);
            kernel::CallGuest(ctx, kDrawBar, {fill_bits, uint32_t(x), uint32_t(y), 100, 8, dark, dark, light, 0});
            game::NoteMenuBar(menu, row, float(x), float(y), 100.0f, 8.0f, BarPointer);
        }
    }
    kernel::Return(ctx, 0, 0);
}

// (menu), each tick: left and right step the selected bar, held they repeat
void VideoTick(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    if (Valid(menu, kSelected + 4)) {
        uint32_t row = kernel::R32(menu + kSelected), actions = kernel::R32(menu + kActions);
        if (Valid(row, 0x18) && BarOf(kernel::R32(row + 0x14))) {
            uint32_t id = kernel::R32(row + 0x14);
            if (actions & 0x40000) SetBar(id, g_value[id] - 1);
            else if (actions & 0x80000) SetBar(id, g_value[id] + 1);
        }
    }
    kernel::Return(ctx, 0, 0);
}

// (menu): Accept applies the choices, Cancel leaves them
void VideoSelect(X86Context& ctx, uint8_t*) {
    uint32_t menu = kernel::R32(ctx.esp + 4);
    if (Valid(menu, kChosen + 4)) {
        uint32_t id = kernel::R32(menu + kChosen);
        if (id == kAcceptId || id == kCancelId) {
            if (id == kAcceptId) Accept();
            Forget();
            Leave(ctx);
        }
    }
    kernel::Return(ctx, 0, 0);
}

}  // namespace

namespace game {
uint32_t VideoBuildCallback() { return g_build_cb; }
std::vector<std::pair<uint32_t, uint32_t>> DisplayModes() { return ListModes(); }
std::string DisplayModeName(uint32_t w, uint32_t h) { return ModeText(w, h); }
}  // namespace game

// the Video Options row right after the Audio/Video row of the Settings and pause menus (and Unlock Everything after
// View Gallery)
X86_EXTERN_FUNC(__imp__sub_00079D60);
X86_FUNC(sub_00079D60) {
    uint32_t caller = kernel::R32(ctx.esp), slot = ctx.esp + 20, callback = kernel::R32(slot);
    bool settings = caller == kFromSettings && callback == kSettingsAudio;
    bool paused = caller == kFromPause && callback == kPauseAudio;
    bool story = game::StoryOnlineRow(ctx);  // the main menu's Story row opens Local / Online
    __imp__sub_00079D60(ctx, base);
    if (story) kernel::W32(slot, 0);
    game::AddUnlockRow(ctx, caller);
    // not in online co-op: both machines run the menus, which would set one player's display on both (F1 instead)
    if (!(settings || paused) || lockstep::Active() || !Prepare()) return;
    kernel::CallGuest(ctx, kAddRow, {g_row_text, 0xFFFFFFFF, 0, 0, g_open_cb, 0});
}

// Audio/Video Options is Audio Options: the English texts replaced, the other languages' kept; Mansion of Madness's lost
// lines (game/mansion_cut.cpp)
X86_EXTERN_FUNC(__imp__sub_0003D7B0);
X86_FUNC(sub_0003D7B0) {
    uint32_t id = kernel::R32(ctx.esp + 4);
    if (uint32_t lost = game::MansionText(id)) return kernel::Return(ctx, lost, 0);
    __imp__sub_0003D7B0(ctx, base);
    if (std::find(std::begin(kAudioVideoTexts), std::end(kAudioVideoTexts), id) == std::end(kAudioVideoTexts)) return;
    if (!Valid(ctx.eax, 12) || !Prepare()) return;
    const char* text = static_cast<const char*>(kernel::HostPtr(ctx.eax));
    if (std::strcmp(text, "Audio/Video Options") == 0) ctx.eax = g_audio_title;
    else if (std::strcmp(text, "Audio/Video") == 0) ctx.eax = g_audio_short;
}
