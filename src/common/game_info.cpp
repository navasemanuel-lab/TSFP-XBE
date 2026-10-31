#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/controls.h"
#include "common/mods.h"
#include "common/mouse.h"
#include "common/options.h"
#include "common/panel.h"
#include "core/input.h"
#include "gpu/backend.h"
#include "net/lockstep.h"
#include "xdk/d3d8.h"

namespace game {

void InstallBriefingTab();  // game/briefing.cpp
void StartCoopOnline();     // game/coop_online.cpp
void ApplyWeaponFov();      // game/fov.cpp
void InitFont();            // game/ts2_style.cpp
void InitSaveSignatures();  // game/save_signatures.cpp
void StartDiscord();        // game/discord.cpp
void StopDiscord();

namespace {

// after the settings load, before the window and boot
void Init() {
    input::ReservePort(Opt().keyboard_own_port ? 0 : -1);
    // the early Cortez (game/beta_cortez.cpp) is put aside for now: always the game's
    Opt().cortez_model = 0;
    // always 60 frames a second, as the game steps, none drawn between them: the console's 30 and frame
    // interpolation are no longer offered (the frames between were never clean, the pieces paired by a guess)
    Opt().fps_60 = true;
    app::Get().frame_interp = 1;
    d3d::g_pace_finish = false;
    // the lens flares and light coronas are tested with points, counted one console pixel each as on the console: no
    // steadying of the counts is needed
    gpu::g_report_hold = 1;
    // the key bindings, the defaults written into the settings file so they can be changed there
    size_t before = Opt().keys.size();
    controls::LoadKeys();
    if (Opt().keys.size() != before) app::Save();
    StartDiscord();
    // online co-op: the settings that change how the game plays out are the host's on both machines
    static const lockstep::Pin pins[] = {{"skip_intros", &Opt().skip_intros, nullptr},
                                         {"skip_any_cutscene", &Opt().skip_any_cutscene, nullptr},
                                         {"fast_loading", &Opt().fast_loading, nullptr},
                                         {"player_names", &Opt().player_names, nullptr},
                                         {"rain", &Opt().rain, nullptr},
                                         {"blood", &Opt().blood, nullptr},
                                         // what the game loads and where the cameras look: the guest's own
                                         // drifted the games apart (Javi's session, 2026-10-29: at frame 60)
                                         {"hands", &Opt().hands, nullptr},
                                         {"font", nullptr, &Opt().font},
                                         {"hud_style", nullptr, &Opt().hud_style},
                                         {"skyline_model", nullptr, &Opt().skyline_model},
                                         {"story_character_p1", nullptr, &Opt().story_character[0]},
                                         {"story_character_p2", nullptr, &Opt().story_character[1]},
                                         {"language", nullptr, &app::Get().language},
                                         {"fov", nullptr, &Opt().fov},
                                         {"fov_p2", nullptr, &Opt().player_fov[0]},
                                         {"fov_p3", nullptr, &Opt().player_fov[1]},
                                         {"fov_p4", nullptr, &Opt().player_fov[2]},
                                         {"weapon_fov", nullptr, &Opt().weapon_fov},
                                         {"weapon_fov_p2", nullptr, &Opt().player_weapon_fov[0]},
                                         {"weapon_fov_p3", nullptr, &Opt().player_weapon_fov[1]},
                                         {"weapon_fov_p4", nullptr, &Opt().player_weapon_fov[2]}};
    lockstep::Pins(pins, int(std::size(pins)));
    // the mods switched on, by folder and file count
    std::string on;
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const mods::Mod& m : mods::List()) {
        if (!m.on) continue;
        std::string one = m.folder + "#" + std::to_string(m.files);
        for (char c : one + ";") hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
        on += (on.empty() ? "" : ", ") + m.name;
    }
    lockstep::Content(hash, on.empty() ? "none" : on);
    StartCoopOnline();
}

// each frame inside the host's ImGui frame. The host hands window messages to ImGui first, and ImGui answers
// WM_SETCURSOR with its arrow: the cursor is the mouse module's (hidden in play, an arrow in the menus)
void Ui(double fps) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    panel::Ui(fps);
}

// the game window is up, ImGui made
void Window(void* hwnd) {
    mouse::Start(hwnd);
    panel::Start();
}

// the image is in memory, its entry point not yet run
void Loaded() {
    InstallBriefingTab();
    ApplyWeaponFov();
    InitFont();
    InitSaveSignatures();
}

// the settings file's [keys] table and the Discord application
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    if (auto id = t["discord_app_id"].value<std::string>()) Opt().discord_app_id = *id;
    if (auto image = t["discord_image"].value<std::string>()) Opt().discord_image = *image;
    if (auto a = t["net_address"].value<std::string>()) Opt().net_address = *a;
    if (auto p = t["net_password"].value<std::string>()) Opt().net_password = *p;
    if (auto f = t["ts2_folder"].value<std::string>()) Opt().ts2_folder = *f;
    if (auto f = t["ts4_folder"].value<std::string>()) Opt().ts4_folder = *f;
    if (auto f = t["preview_path"].value<std::string>()) Opt().preview_path = *f;
    if (auto f = t["mods_off"].value<std::string>()) Opt().mods_off = *f;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) Opt().keys[std::string(id.str())] = *v;
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    toml::table keys;
    for (size_t i = 0; const char* id = controls::ActionIds(i); i++) {
        auto it = Opt().keys.find(id);
        if (it != Opt().keys.end()) keys.insert(id, it->second);
    }
    t.insert_or_assign("keys", keys);
    t.insert_or_assign("discord_app_id", Opt().discord_app_id);
    t.insert_or_assign("discord_image", Opt().discord_image);
    t.insert_or_assign("net_address", Opt().net_address);
    t.insert_or_assign("net_password", Opt().net_password);
    t.insert_or_assign("ts2_folder", Opt().ts2_folder);
    t.insert_or_assign("ts4_folder", Opt().ts4_folder);
    t.insert_or_assign("preview_path", Opt().preview_path);
    t.insert_or_assign("mods_off", Opt().mods_off);
}

// window thread: the mouse and its cursor
bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (msg == WM_DESTROY) {
        StopDiscord();    // the presence cleared as the game closes
        lockstep::Bye();  // and a co-op partner told
    }
    if (panel::Message(msg, wp, lp, result)) return true;
    // Alt+Enter: a window, or back to the full screen mode it came from; kept in the settings file
    constexpr intptr_t kAltDown = 1 << 29, kRepeat = 1 << 30;
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & kAltDown)) {
        if (!(lp & kRepeat)) {
            static app::DisplayMode full = app::DisplayMode::Borderless;
            app::Settings& s = app::Get();
            if (s.display_mode == app::DisplayMode::Windowed) {
                s.display_mode = full;
            } else {
                full = s.display_mode;
                s.display_mode = app::DisplayMode::Windowed;
            }
            app::ApplyDisplay();
            app::Save();
        }
        *result = 0;
        return true;
    }
    return mouse::Message(msg, wp, lp, result);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

uint32_t& Fov(uint32_t slot) { return slot && slot < 4 ? Opt().player_fov[slot - 1] : Opt().fov; }
uint32_t& WeaponFov(uint32_t slot) { return slot && slot < 4 ? Opt().player_weapon_fov[slot - 1] : Opt().weapon_fov; }

}  // namespace game

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"TimeSplitters Future Perfect",  // window and splash
    .short_title = L"TSFP",                    // message boxes
    .name = L"TSFP",                           // window classes, crash dumps, handover event
    .log = "tsfp",                             // log file prefix
    .settings = "settings.toml",               // settings file beside the exe
    .splash = L"splash",                       // optional splash.png or splash.bmp beside the exe
    .missing = L"Put TimeSplitters Future Perfect in the assets folder: its disc image (.iso), or default.xbe with its pak and xmv folders beside it.",
    .images = {{"default.xbe", "TSFP"}},       // each recompiled xbe and its exe, for title launches
    .options = {{"skip_intros", &game::Opt().skip_intros, nullptr},
                {"skip_any_cutscene", &game::Opt().skip_any_cutscene, nullptr},
                {"fast_loading", &game::Opt().fast_loading, nullptr},
                {"player_names", &game::Opt().player_names, nullptr},
                {"fov", nullptr, &game::Opt().fov}, {"weapon_fov", nullptr, &game::Opt().weapon_fov},
                {"fov_p2", nullptr, &game::Opt().player_fov[0]},
                {"weapon_fov_p2", nullptr, &game::Opt().player_weapon_fov[0]},
                {"fov_p3", nullptr, &game::Opt().player_fov[1]},
                {"weapon_fov_p3", nullptr, &game::Opt().player_weapon_fov[1]},
                {"fov_p4", nullptr, &game::Opt().player_fov[2]},
                {"weapon_fov_p4", nullptr, &game::Opt().player_weapon_fov[2]},
                {"mouse_sensitivity", nullptr, &game::Opt().mouse_sensitivity},
                {"mouse_vertical", nullptr, &game::Opt().mouse_vertical},
                {"invert_mouse", &game::Opt().invert_mouse, nullptr},
                {"stick_deadzone", nullptr, &game::Opt().stick_deadzone},
                {"stick_curve", nullptr, &game::Opt().stick_curve},
                {"gyro_aim", &game::Opt().gyro_aim, nullptr},
                {"gyro_speed", nullptr, &game::Opt().gyro_speed},
                {"hands", &game::Opt().hands, nullptr},
                {"show_fps", &game::Opt().show_fps, nullptr},
                {"show_stats", &game::Opt().show_stats, nullptr},
                {"log_stats", &game::Opt().log_stats, nullptr},
                {"rain", &game::Opt().rain, nullptr},
                {"blood", &game::Opt().blood, nullptr},
                {"font", nullptr, &game::Opt().font},
                {"hud_style", nullptr, &game::Opt().hud_style},
                {"cortez_model", nullptr, &game::Opt().cortez_model},
                {"skyline_model", nullptr, &game::Opt().skyline_model},
                {"story_character_p1", nullptr, &game::Opt().story_character[0]},
                {"story_character_p2", nullptr, &game::Opt().story_character[1]},
                {"keyboard_own_port", &game::Opt().keyboard_own_port, nullptr},
                {"discord", &game::Opt().discord, nullptr},
                {"net_port", nullptr, &game::Opt().net_port}},
    .load = game::Load,                        // reads more from the settings file's toml::table
    .save = game::Save,                        // writes more into it
    .init = game::Init,                        // after the settings load, before the window and boot
    .loaded = game::Loaded,                    // image in memory, before its entry point runs
    .window = game::Window,                    // the game window is up
    .message = game::Message,                  // window messages first; true takes one
    .ui = game::Ui,                            // ImGui drawing each frame
    .pad = controls::Pad,                      // controllers, and the keyboard on port 1
    .rumble = nullptr,                         // changes a rumble before it is set
    .launcher = L"TSFP Launcher.exe",          // brings the game into assets when it is missing
};
