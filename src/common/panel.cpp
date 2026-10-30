// The settings panel: F1, or Back and Start together on a pad, opens it over the game, after the in-game settings
// panel of lmplied's TimeSplitters recompilation (lmplied/TSFPRecomp, docs/technical/in-game-menu.md), laid out as
// Tenkaichi3Decomp's (z3xox/Tenkaichi3Decomp, port/src/gs/ui.cpp): a window in the middle of the screen with a tab for
// each section, each setting a line of its label and its control, what it does shown only while the mouse is over it
// (or the pad's selection is on it). A change applies at once, except a few that apply at the next start, which their
// line says. The settings file is written on closing, and a rebound control at once. It is drawn with ImGui over the
// frame, after the frame is scaled to the window. While it is open the cursor is free and the game sees idle pads, but
// the game keeps running. After it closes, the keys and buttons that closed it reach the game only once they are let
// go. The Co-op tab chooses Story's players' characters and shows an online co-op session (net/lockstep.h), and while
// one runs the settings that change how the game plays out are the host's, shown fixed; the session's notices (the
// other player late or gone) are drawn over the game.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/controls.h"
#include "common/mods.h"
#include "common/mouse.h"
#include "common/options.h"
#include "common/panel.h"
#include "core/input.h"
#include "core/log.h"
#include "net/lockstep.h"
#include "xdk/d3d8.h"

namespace game {
void ApplyWeaponFov();    // game/fov.cpp
void ApplyScreenShape();  // game/fov.cpp
void ApplyFont();         // game/ts2_style.cpp
std::vector<std::pair<uint32_t, uint32_t>> DisplayModes();  // game/video_menu.cpp: the desktop's, then the sizes
std::string DisplayModeName(uint32_t w, uint32_t h);
std::string CoopNotice();  // game/coop_online.cpp
std::vector<std::string> StoryCharacterNames();  // game/story_characters.cpp
}  // namespace game

namespace panel {

namespace {

constexpr int kToggleKey = VK_F1;
constexpr float kFontPx = 48;   // the letters made at this size
constexpr float kTextPx = 15;   // drawn at this a 1080 line screen, more on a taller one
constexpr int kSlots = 3;       // keys shown for each action

constexpr ImU32 kAccent = IM_COL32(102, 194, 255, 255);
constexpr ImU32 kPanelBg = IM_COL32(11, 14, 21, 245);

std::atomic<bool> g_open{false};
std::atomic<bool> g_hold[4] = {};
std::atomic<bool> g_save{false};
bool g_was_open = false;
int g_tab = 0;
int g_force_tab = -1;  // a tab chosen with the shoulder buttons, selected at the next frame
float g_scale = 1;
ImFont* g_font = nullptr;
ImGuiStyle g_base_style;
bool g_styled = false;
std::vector<std::pair<uint32_t, uint32_t>> g_modes;
std::vector<std::string> g_mode_names;
// the Controls tab: each action's keys as the panel last set them, the key slot waiting for a key
std::vector<std::vector<std::string>> g_keys;
int g_capture = -1, g_capture_slot = 0;
bool g_armed = false;     // the keys held when it started are let go
bool g_cooldown = false;  // a key was just bound: the rows take no press until it is let go
std::string g_moved;      // what the last binding took from another action
int g_nav_flags = 0;

float Px(float v) { return v * g_scale; }

void SetOpen(bool open) {
    if (g_open.exchange(open) == open) return;
    if (!open) {
        for (auto& h : g_hold) h = true;
        g_save = true;
    }
    mouse::SetPanel(open);
    LOGR("panel: %s", open ? "open" : "closed, the settings saved");
}

std::vector<std::string> Split(const std::string& keys) {
    std::vector<std::string> out;
    for (size_t start = 0; start <= keys.size();) {
        size_t comma = std::min(keys.find(',', start), keys.size());
        if (comma > start) out.push_back(keys.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& keys) {
    std::string out;
    for (const std::string& k : keys) out += (out.empty() ? "" : ",") + k;
    return out;
}

// the panel was just opened: what it shows that is slow to find
void Stage() {
    g_modes = game::DisplayModes();
    g_mode_names.clear();
    for (auto [w, h] : g_modes) g_mode_names.push_back(game::DisplayModeName(w, h));
    g_keys.clear();
    for (size_t i = 0; const char* id = controls::ActionIds(i); i++) {
        auto it = game::Opt().keys.find(id);
        g_keys.push_back(Split(it != game::Opt().keys.end() ? it->second : controls::ActionDefaults(i)));
    }
    g_capture = -1;
    g_moved.clear();
}

// the panel's look, its sizes at g_scale
void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10;
    st.ChildRounding = 6;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.PopupRounding = 6;
    st.ScrollbarRounding = 6;
    st.WindowPadding = {12, 10};
    st.FramePadding = {8, 4};
    st.CellPadding = {6, 3};
    st.ItemSpacing = {8, 6};
    st.ItemInnerSpacing = {6, 4};
    st.ScrollbarSize = 10;
    st.GrabMinSize = 12;
    st.WindowBorderSize = 1;
    st.PopupBorderSize = 1;
    st.WindowTitleAlign = {0.5f, 0.5f};
    st.ScaleAllSizes(g_scale);
    ImVec4* c = st.Colors;
    const ImVec4 accent(0.40f, 0.76f, 1.0f, 1.0f), dim(0.40f, 0.76f, 1.0f, 0.55f);
    c[ImGuiCol_Text] = ImVec4(0.89f, 0.91f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.55f, 0.64f, 1.0f);
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(kPanelBg);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.17f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.12f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.23f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.24f, 0.32f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.29f, 0.38f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_ButtonHovered] = dim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.40f, 0.76f, 1.0f, 0.30f);
    c[ImGuiCol_HeaderHovered] = dim;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.15f, 0.20f, 1.0f);
    c[ImGuiCol_TabHovered] = dim;
    c[ImGuiCol_TabSelected] = ImVec4(0.40f, 0.76f, 1.0f, 0.80f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TableBorderLight] = c[ImGuiCol_TableBorderStrong] = ImVec4(0.17f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.27f, 0.32f, 0.41f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.17f, 0.20f, 0.27f, 1.0f);
}

// what a line does, in a box by the pointer while it is over the line or the pad's selection is on it
void Tip(const char* help, bool hot) {
    if (!help || !(hot || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))) return;
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// a line of a tab's table: its label (and a note dimmed after it) at the left, its control filling the right. Line()
// leaves the cursor in the control's cell; Done() puts up what it does while either cell is pointed at
bool g_label_hot = false;
void Line(const char* label, const char* note = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    g_label_hot = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    if (note) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", note);
        g_label_hot |= ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void Done(const char* help) { Tip(help, g_label_hot); }

// a tab's lines: two columns, the labels' as wide as they need
bool BeginLines(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    return true;
}
void EndLines() { ImGui::EndTable(); }

// one of names, in a list that opens
bool Choice(const char* label, int& index, const std::vector<std::string>& names, const char* help,
            const char* note = nullptr) {
    int n = int(names.size());
    index = std::clamp(index, 0, n - 1);
    ImGui::PushID(label);
    Line(label, note);
    int was = index;
    if (ImGui::BeginCombo("##v", names[index].c_str())) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i].c_str(), i == index)) index = i;
            if (i == index) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    Done(help);
    ImGui::PopID();
    if (index != was) g_save = true;
    return index != was;
}

bool Switch(const char* label, bool& on, const char* help, const char* note = nullptr) {
    ImGui::PushID(label);
    Line(label, note);
    bool changed = ImGui::Checkbox("##v", &on);
    Done(help);
    ImGui::PopID();
    if (changed) g_save = true;
    return changed;
}

// one of values, by the names made of them; a value of the settings file among none of them is put in its order
bool Values(const char* label, uint32_t& value, std::vector<uint32_t> values, std::string (*name)(uint32_t),
            const char* help, const char* note = nullptr) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.insert(std::upper_bound(values.begin(), values.end(), value), value);
    std::vector<std::string> names;
    for (uint32_t v : values) names.push_back(name(v));
    int i = int(std::find(values.begin(), values.end(), value) - values.begin());
    if (!Choice(label, i, names, help, note)) return false;
    value = values[i];
    return true;
}

// a number from lo to hi on a slider; a right click puts back its default. live: it applies while dragged, otherwise
// once let go
bool Slider(const char* label, uint32_t& value, uint32_t lo, uint32_t hi, uint32_t def, const char* format,
            const char* help, bool live = true) {
    ImGui::PushID(label);
    Line(label);
    static int dragged = 0;
    static const void* owner = nullptr;
    int v = owner == &value ? dragged : int(value);
    char shown[64];
    std::snprintf(shown, sizeof shown, format, v);
    if (uint32_t(v) == def) std::strncat(shown, "  (Default)", sizeof shown - std::strlen(shown) - 1);
    ImGui::SliderInt("##v", &v, int(lo), int(hi), shown, ImGuiSliderFlags_AlwaysClamp);
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) g_save = true;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) v = int(def), g_save = true;
    Done(help);
    ImGui::PopID();
    uint32_t was = value;
    if (live || !active) {
        value = uint32_t(v);
        if (owner == &value) owner = nullptr;
    } else {
        owner = &value;
        dragged = v;
    }
    return value != was;
}

bool Button(const char* label, const char* help) {
    ImGui::PushID(label);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    bool press = ImGui::Button(label, {-FLT_MIN, 0});
    Tip(help, false);
    ImGui::PopID();
    return press;
}

void Fixed(const char* label, const std::string& value, const char* help) {
    Line(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", value.c_str());
    Done(help);
}

// a text typed in
void Edit(const char* label, std::string& value, const char* help, bool hidden = false) {
    ImGui::PushID(label);
    Line(label);
    char buf[260];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    if (ImGui::InputText("##v", buf, sizeof buf, hidden ? ImGuiInputTextFlags_Password : 0)) value = buf;
    if (ImGui::IsItemDeactivatedAfterEdit()) g_save = true;
    Done(help);
    ImGui::PopID();
}

std::string Filtering(uint32_t v) { return v ? std::to_string(v) + "x anisotropic" : "As the game asks"; }
std::string Multisampling(uint32_t v) { return v ? std::to_string(v) + "x" : "Off"; }

void DisplayTab() {
    app::Settings& s = app::Get();
    game::Options& o = game::Opt();
    int mode = int(s.display_mode);
    if (Choice("Display mode", mode, {"Fullscreen", "Borderless", "Windowed"},
               "Fullscreen takes the monitor's mode; borderless covers the desktop; windowed keeps a frame. Alt+Enter "
               "switches too.")) {
        s.display_mode = app::DisplayMode(mode);
        app::ApplyDisplay();
    }
    auto at = std::find(g_modes.begin(), g_modes.end(), std::make_pair(s.width, s.height));
    if (at == g_modes.end()) {
        g_modes.push_back({s.width, s.height});
        g_mode_names.push_back(game::DisplayModeName(s.width, s.height));
        at = g_modes.end() - 1;
    }
    int res = int(at - g_modes.begin());
    if (Choice("Resolution", res, g_mode_names,
               "The window's size, or the screen's mode in fullscreen. Desktop is the monitor's own.")) {
        s.width = g_modes[res].first;
        s.height = g_modes[res].second;
        app::ApplyDisplay();
        game::ApplyScreenShape();
    }
    Slider("Render scale", s.render_percent, 1, 200, 100, "%d%%",
           "The scene drawn at this part of the output's height. Under 100% is faster; over it the scene is drawn "
           "larger and shrunk to the screen, smoother edges for more GPU work (200% on a 1080p screen draws at 4K). "
           "Applies when the slider is let go; a right click puts back 100%.",
           false);
    int aspect = int(s.aspect);
    if (Choice("Aspect ratio", aspect, {"Auto", "4:3", "16:9", "16:10", "21:9", "32:9"},
               "The picture's shape. Auto follows the window; 4:3 is the console's.")) {
        s.aspect = uint32_t(aspect);
        app::ApplyDisplay();
        game::ApplyScreenShape();
    }
    Slider("Brightness", s.brightness, 50, 150, 100, "%d%%",
           "Lifts or lowers the darks and middle tones; white and black stay. A right click puts back 100%.");
    Switch("Vertical sync", s.vsync, "Frames wait for the display's refresh: no tearing, a little more latency.");
    Switch("Low latency", s.low_latency,
           "One frame waits for the display instead of two: what you do shows a frame sooner, above all with vertical "
           "sync on or when the GPU is behind. Off gives a GPU that is just short of the frame rate a frame of room.");
    Switch("Show frame rate", o.show_fps, "The frames a second in the top left corner.");
}

void GraphicsTab() {
    app::Settings& s = app::Get();
    game::Options& o = game::Opt();
    // System Link and online play: everyone at the game's own angles, the lines gone. Online co-op: each machine's own.
    if (!game::FovLocked()) {
        bool coop = lockstep::Active();
        if (Slider("Field of view", game::Fov(0), 40, 100, 55, "%d\xC2\xB0",
                   coop ? "How much of the world your view takes in. Yours alone: the other player keeps theirs. A right "
                          "click puts back 55."
                        : "How much of the world the view takes in, top to bottom. Wider shows more at the edges. A "
                          "right click puts back 55."))
            game::ApplyWeaponFov();
        if (Slider("Weapon field of view", game::WeaponFov(0), 30, 70, 45, "%d\xC2\xB0",
                   coop ? "Your held weapon's angle. Yours alone: the other player keeps theirs. A right click puts "
                          "back 45."
                        : "The held weapon's angle, whatever the view's. Lower draws it bigger. A right click puts back "
                          "45."))
            game::ApplyWeaponFov();
    }
    Switch("Hands", o.hands, "The arms holding the weapon. Off, the weapon alone, as in TimeSplitters 2.");
    const char* limited = s.low_profile ? "(4x at most on this GPU)" : nullptr;
    Values("Texture sharpness", s.aniso, {0, 2, 4, 8, 16}, Filtering,
           "Anisotropic filtering: textures stay sharp at a slant.", limited);
    Values("MSAA", s.msaa, {0, 2, 4, 8}, Multisampling,
           "Multisample antialiasing: smooths the edges of what is drawn while keeping the picture sharp. It does not "
           "smooth cut-out edges such as fences and leaves. 8x costs the most, above all at high resolutions.",
           s.low_profile ? "(off on this GPU)" : nullptr);
    int aa = int(std::min<uint32_t>(s.edge_aa, 2));
    if (Choice("Edge smoothing", aa, {"Off", "FXAA", "SMAA"},
               "Smooths the edges of the final picture, cut-out ones such as fences and leaves too, and can go with "
               "MSAA. FXAA costs almost nothing, though it softens text a little; SMAA follows each edge's shape, "
               "smoother on slants and leaving straight lines and text sharp."))
        s.edge_aa = uint32_t(aa);
    Switch("Seal seams", s.seal_seams,
           "Closes the hairline cracks between pieces of the levels that open up at high resolutions, through which "
           "what is behind shows and flickers as the view moves. Solid walls and models cover every pixel they touch.");
    int ao = int(std::min<uint32_t>(s.ao, 2));
    if (Choice("Ambient occlusion", ao, {"Off", "SSAO", "GTAO"},
               "Soft contact shadows where surfaces meet: corners, under ledges, around props. SSAO is the lighter "
               "one; GTAO follows the shape around each point more closely and darkens more naturally, at a little "
               "more cost. Both are worked out at half size."))
        s.ao = uint32_t(ao);
    Switch("Console depth ties", s.depth_ties,
           "Surfaces laid almost on one another (decals, trims, floors over floors) tie in depth as on the console, "
           "the one drawn last showing, instead of flickering near and far as the view moves. Works without MSAA.");
    Switch("Fog", s.fog, "The distance fog of levels such as the mansion. Off, the far scenery is drawn clear.");
    if (lockstep::Active())
        Fixed("Rain", o.rain ? "On  (the host's)" : "Off  (the host's)", "Online co-op plays on the host's settings.");
    else
        Switch("Rain", o.rain,
               "The rain around the view and the drops running down the screen, as in the mansion. Off saves its "
               "thousands of drops a frame; the sound stays.");
    if (lockstep::Active())
        Fixed("Blood", o.blood ? "On  (the host's)" : "Off  (the host's)", "Online co-op plays on the host's settings.");
    else
        Switch("Blood", o.blood, "The blood sprayed by a hit and the splats and pools it leaves on walls and floors.");
}

void MouseLines() {
    game::Options& o = game::Opt();
    Slider("Mouse sensitivity", o.mouse_sensitivity, 5, 500, 100, "%d",
           "How far the view turns for a move of the mouse. 100 is 0.1 degrees a count. A right click puts back 100.");
    Slider("Up and down speed", o.mouse_vertical, 25, 200, 100, "%d%%",
           "Looking up and down, against the sideways speed. A right click puts back 100%.");
    Switch("Invert up and down", o.invert_mouse, "Moving the mouse forward looks down.");
}

const char* ActionLabel(const char* id) {
    static const std::pair<const char*, const char*> kLabels[] = {
        {"action", "Action / Accept"},   {"back_melee", "Melee / Back"},     {"reload", "Reload"},
        {"y", "Y button"},               {"black", "Black button"},          {"white", "White button"},
        {"pause", "Pause"},              {"objectives", "Objectives"},       {"grenade", "Grenade"},
        {"fire", "Fire"},                {"weapon_mode", "Weapon mode / zoom in"},
        {"grenade_type", "Grenade type / zoom out"},                         {"previous_weapon", "Previous weapon"},
        {"next_weapon", "Next weapon"},  {"crouch", "Crouch"},               {"aim", "Aim"},
        {"weapon_1", "Weapon 1"},        {"weapon_2", "Weapon 2"},           {"weapon_3", "Weapon 3"},
        {"weapon_4", "Weapon 4"},        {"weapon_5", "Weapon 5"},           {"weapon_6", "Weapon 6"},
        {"weapon_7", "Weapon 7"},        {"weapon_8", "Weapon 8"},           {"weapon_9", "Weapon 9"},
        {"weapon_10", "Weapon 10"},
    };
    for (auto [i, l] : kLabels)
        if (!std::strcmp(i, id)) return l;
    return id;
}

void Bind(size_t i, const std::vector<std::string>& keys) {
    g_keys[i] = keys;
    std::string joined = Join(keys);
    controls::SetKeys(controls::ActionIds(i), joined);
    g_save = true;
    LOGR("panel: %s = '%s'", controls::ActionIds(i), joined.c_str());
}

// the key slot waiting gets the next key or mouse button, which leaves any other action that had it; Delete clears
// the slot, Escape keeps it
void Capture() {
    if (g_capture < 0 && !g_cooldown) return;
    int down = 0;
    for (int vk = 1; vk < 255 && !down; vk++)
        if (GetAsyncKeyState(vk) < 0 && (vk == VK_DELETE || !controls::KeyName(vk).empty())) down = vk;
    if (g_cooldown) {
        if (!down) g_cooldown = false;
        return;
    }
    if (!g_armed) {
        g_armed = !down;
        return;
    }
    if (!down) return;
    size_t a = size_t(g_capture);
    std::vector<std::string> keys = g_keys[a];
    size_t slot = size_t(g_capture_slot);
    if (down == VK_DELETE) {
        if (slot < keys.size()) keys.erase(keys.begin() + ptrdiff_t(slot));
        Bind(a, keys);
    } else if (down != VK_ESCAPE) {
        std::string name = controls::KeyName(down);
        g_moved.clear();
        for (size_t j = 0; j < g_keys.size(); j++) {
            if (j == a) continue;
            std::vector<std::string> other = g_keys[j];
            auto it = std::find_if(other.begin(), other.end(), [&](const std::string& k) {
                return !_stricmp(k.c_str(), name.c_str());
            });
            if (it == other.end()) continue;
            other.erase(it);
            Bind(j, other);
            g_moved = name + " was taken from " + ActionLabel(controls::ActionIds(j)) + ".";
        }
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [&](const std::string& k) { return !_stricmp(k.c_str(), name.c_str()); }),
                   keys.end());
        slot = std::min(slot, keys.size());
        if (slot < keys.size()) keys[slot] = name;
        else keys.push_back(name);
        Bind(a, keys);
    }
    g_capture = -1;
    g_cooldown = true;
}

void PadLines() {
    game::Options& o = game::Opt();
    Slider("Stick dead zone", o.stick_deadzone, 0, 30, 8, "%d%%",
           "The right stick's centre that does nothing, round, in place of the game's wider square one. Raise it if "
           "the view drifts with the stick let go. A right click puts back 8%.");
    Slider("Aim curve", o.stick_curve, 100, 250, 150, "%d",
           "The right stick's response: 100 straight; higher turns slower near the centre, for fine aim, and as fast "
           "at the edge. A right click puts back 150.");
    Switch("Gyro aiming", o.gyro_aim,
           "Turning a controller with a gyro (DualSense, DualShock 4, Switch Pro) turns the view, beside the right "
           "stick, held at any tilt. Lay the controller still for a second to stop any drift.");
    Slider("Gyro speed", o.gyro_speed, 50, 800, 200, "%d%%",
           "How far the view turns for a turn of the controller: 100% as far as the controller, 200% twice. A right "
           "click puts back 200%.");
}

void ControlsTab() {
    if (BeginLines("##mouse")) {
        MouseLines();
        PadLines();
        EndLines();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Click a key to bind it again, then press the new key or mouse button.");
    ImGui::TextDisabled("Escape keeps it, Delete clears it; a right click on a key clears it too.");
    if (!g_moved.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "%s", g_moved.c_str());
    bool busy = g_capture >= 0 || g_cooldown;
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##keys", 1 + kSlots, kFlags)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        for (int k = 0; k < kSlots; k++)
            ImGui::TableSetupColumn(k == 0 ? "Key" : k == 1 ? "Second key" : "Third key",
                                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; const char* id = controls::ActionIds(i); i++) {
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ActionLabel(id));
            for (int k = 0; k < kSlots; k++) {
                ImGui::TableNextColumn();
                ImGui::PushID(k);
                bool waiting = g_capture == int(i) && g_capture_slot == k;
                const std::vector<std::string>& keys = g_keys[i];
                std::string shown = waiting ? "press a key..." : size_t(k) < keys.size() ? keys[size_t(k)] : "-";
                if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
                bool press = ImGui::Button((shown + "##slot").c_str(), {-FLT_MIN, 0});
                if (waiting) ImGui::PopStyleColor();
                bool clear = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                if (!busy && press) {
                    g_capture = int(i);
                    g_capture_slot = std::min(k, int(keys.size()));
                    g_armed = false;
                    g_moved.clear();
                } else if (!busy && clear && size_t(k) < keys.size()) {
                    std::vector<std::string> left = keys;
                    left.erase(left.begin() + k);
                    Bind(i, left);
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Move");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("W, A, S, D");
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset controls to this game's defaults") && !busy) {
        for (size_t i = 0; controls::ActionIds(i); i++) Bind(i, Split(controls::ActionDefaults(i)));
        game::Options& o = game::Opt();
        o.mouse_sensitivity = 100;
        o.mouse_vertical = 100;
        o.invert_mouse = false;
        g_moved.clear();
    }
    Tip("Every action's keys as the port first had them, and the mouse at sensitivity 100, up and down 100%, not "
        "inverted.",
        false);
}

void GameplayTab() {
    game::Options& o = game::Opt();
    if (lockstep::Active()) {
        // the session's: they change how the game plays out, both machines play on the host's
        const char* help = "Online co-op plays on the host's settings.";
        auto host = [&](const char* label, bool on) { Fixed(label, on ? "On  (the host's)" : "Off  (the host's)", help); };
        host("Skip intro videos", o.skip_intros);
        host("Skip any cutscene", o.skip_any_cutscene);
        host("Fast loading", o.fast_loading);
        host("Player names in co-op", o.player_names);
    } else {
        Switch("Skip intro videos", o.skip_intros, "The EA and Free Radical logo movies at boot.", "(next start)");
        Switch("Skip any cutscene", o.skip_any_cutscene, "Start or A skips cutscenes the game would not let go of yet.");
        Switch("Fast loading", o.fast_loading, "A loading screen ends when the load does, not when its bar has filled.");
        Switch("Player names in co-op", o.player_names, "Each Story co-op player's name over their head.");
    }
    // the disc has these five: menus and voices (sounds_e, _f, _g, _s, _i)
    static const uint32_t kLanguages[] = {1, 4, 3, 5, 6};
    uint32_t& language = app::Get().language;
    int lang = int(std::find(std::begin(kLanguages), std::end(kLanguages), language) - std::begin(kLanguages));
    if (Choice("Language", lang, {"English", "Francais", "Deutsch", "Espanol", "Italiano"},
               "The menus, text and voices, as the console's language setting chose them.", "(next start)"))
        language = kLanguages[lang];
    Switch("Keyboard on its own player", o.keyboard_own_port,
           "The keyboard and mouse are player 1 and controllers are players 2 to 4. Off, the first controller shares "
           "player 1.",
           "(next start)");
    Switch("Discord status", o.discord, "What you play shown on your Discord profile.", "(next start)");
}

// the look and the early game's content, read from the player's own copies of the other games
void EarlyTab() {
    game::Options& o = game::Opt();
    int font = int(o.font);
    if (Choice("Font", font, {"Future Perfect", "TimeSplitters 2", "Arial Bold (early)"},
               "The text's letters. TimeSplitters 2's are read from your own copy of it for Xbox (its folder below); "
               "Arial Bold is the early Future Perfect's subtitle font.")) {
        o.font = uint32_t(font);
        game::ApplyFont();
    }
    int hud = int(o.hud_style);
    if (Choice("HUD style", hud, {"Future Perfect", "Early (TimeSplitters 2)", "TimeSplitters 1"},
               "Early: TimeSplitters 2's HUD, as the preview had it: the health bars of allies and bosses in the top "
               "right corner, the health and armour arcs, and the ammunition with its rounds and icons (these read "
               "from your TimeSplitters 2, its folder below, when a level starts). TimeSplitters 1: the health and "
               "armour bars across the top left corner and the green cross crosshair."))
        o.hud_style = uint32_t(hud);
    Edit("TimeSplitters 2 folder", o.ts2_folder,
         "The folder of your TimeSplitters 2 for Xbox, with its default.xbe and data folder. Nothing is copied from "
         "it; it is read at start. Empty: the ts2 folder beside the game, when there is one.");
    int skyline = int(o.skyline_model);
    if (Choice("Breaking and Entering skyline", skyline, {"Future Perfect", "Early (2004)"},
               "Early: the yellow towers at sunset around the rooftop, with the NEW-GEN, SIRIUS, PLAZA and SUN-STAR "
               "signs, from your own preview disc of Future Perfect for PS2 (below). Takes effect when a level starts."))
        o.skyline_model = uint32_t(skyline);
    Edit("Future Perfect preview (PS2)", o.preview_path,
         "Your disc image (.iso) of the November 2004 preview of Future Perfect for PS2, or a folder of its files "
         "(with its PAK folder). Nothing is copied from it; the early skyline is made from it when first needed. "
         "Empty: the preview folder beside the game, when there is one.");
}

// Story's players' characters, alone or in co-op
void StoryCharacterLines() {
    game::Options& o = game::Opt();
    std::vector<std::string> names = game::StoryCharacterNames();
    const char* help = "Who this player is in Story, alone or in co-op: each mission's own, or any of the Arcade "
                       "characters. Online, both of you choose the same. Takes effect from the next mission.";
    for (int p = 0; p < 2; p++) {
        int i = int(std::min<uint32_t>(o.story_character[p], uint32_t(names.size() - 1)));
        if (Choice(p ? "Story player 2" : "Story player 1", i, names, help)) o.story_character[p] = uint32_t(i);
    }
}

// Co-op: Story's players' characters; an online session (the main menu's Story > Host or Join Online Story, or
// --host and --join) started, shown and left here
void CoopTab() {
    StoryCharacterLines();
    game::Options& o = game::Opt();
    if (lockstep::Active()) {
        Fixed("Session", lockstep::Status(), "Each of you plays your own player full screen; the game is the same on both.");
        if (Button("Leave co-op", "Starts the game again on its own, with your own saves."))
            lockstep::Restart("", "");
        return;
    }
    Edit("Host's address", o.net_address,
         "The address of the player who hosts (a VPN's like Radmin's, or their public one with the port forwarded), "
         "for Join. The host leaves it as it is.");
    Edit("Password", o.net_password, "Optional: the host's, the same on both.", true);
    Fixed("Port", std::to_string(o.net_port), "UDP. net_port in the settings file to change it.");
    if (Button("Host online co-op", "Starts the game again waiting for player 2; you are player 1 and your saves are "
                                    "the session's. Also in the main menu: Story > Host Online Story.")) {
        app::Save();
        lockstep::Restart("host:" + std::to_string(o.net_port), o.net_password);
    }
    if (!o.net_address.empty() &&
        Button("Join online co-op", "Starts the game again joining the host's address as player 2, with the host's "
                                    "saves. Also in the main menu: Story > Join Online Story.")) {
        app::Save();
        lockstep::Restart("join:" + o.net_address + ":" + std::to_string(o.net_port), o.net_password);
    }
}

// Mods: the early content, then the folders in mods beside the game, each switched on or off
void ModsTab() {
    EarlyTab();
    static std::vector<game::mods::Mod> mods = game::mods::List();
    if (Button("Look again", "Reads the mods folder again, for a mod added or changed while the game runs."))
        mods = game::mods::List(true);
    if (mods.empty()) {
        Fixed("No mods", "(point here)",
              "Put each mod in a folder of its own in the mods folder beside the game: mods\\<mod>\\files\\ holds files "
              "by their path in the game's paks (assets\\ts\\textures\\xbox\\<name>.xbt, a model's .xbr...), used in "
              "place of the game's own. mod.toml beside files may give name, author and description.");
        return;
    }
    for (game::mods::Mod& m : mods) {
        std::string help = (m.description.empty() ? std::string("No description.") : m.description) + "\n\nFolder: mods\\" +
                           m.folder + ", " + std::to_string(m.files) + " files" +
                           (m.author.empty() ? std::string() : ", by " + m.author) +
                           ".\nA change is seen from the next level on. In online co-op both players need the same mods.";
        if (Switch(m.name.c_str(), m.on, help.c_str())) game::mods::SetOn(m.folder, m.on);
    }
}

// a session's notice over the game: the other player late or gone
void DrawNotice() {
    std::string text = lockstep::Notice();
    if (text.empty()) text = game::CoopNotice();
    if (text.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    float size = std::clamp(io.DisplaySize.y / 900.0f, 0.75f, 2.5f) * 30;
    ImVec2 ext = g_font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p{(io.DisplaySize.x - ext.x) / 2, io.DisplaySize.y * 0.12f};
    dl->AddRectFilled({p.x - size, p.y - size * 0.4f}, {p.x + ext.x + size, p.y + ext.y + size * 0.4f},
                      kPanelBg, size * 0.2f);
    dl->AddText(g_font, size, p, kAccent, text.c_str());
}

// the frame rate in the top left corner, over everything
void DrawFps(double fps) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    ImGuiIO& io = ImGui::GetIO();
    float size = std::clamp(io.DisplaySize.y / 900.0f, 0.75f, 2.5f) * 24;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p{size * 0.6f, size * 0.4f};
    dl->AddText(g_font, size, {p.x + 1, p.y + 1}, IM_COL32(0, 0, 0, 200), text);
    dl->AddText(g_font, size, p, kAccent, text);
}

struct Tab {
    const char* name;
    void (*draw)();
    bool lines;  // its settings are lines of the two column table
};

constexpr Tab kTabs[] = {
    {"Display", DisplayTab, true},  {"Graphics", GraphicsTab, true}, {"Controls", ControlsTab, false},
    {"Gameplay", GameplayTab, true}, {"Mods", ModsTab, true},
    {"Co-op", CoopTab, true},
};
constexpr int kTabCount = int(std::size(kTabs));

// the window: the tabs, and a last line that is always there
void Draw() {
    ImGuiIO& io = ImGui::GetIO();
    g_scale = std::clamp(io.DisplaySize.y / 1080.0f, 1.0f, 2.4f);
    ImGuiStyle saved = ImGui::GetStyle();
    float saved_font = io.FontGlobalScale;
    if (!g_styled) {
        g_base_style = saved;
        g_styled = true;
    }
    ImGui::GetStyle() = g_base_style;
    Style();
    io.FontGlobalScale = g_scale * kTextPx / kFontPx;

    if (g_capture < 0 && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_force_tab = (g_tab + kTabCount - 1) % kTabCount;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_force_tab = (g_tab + 1) % kTabCount;
        if (ImGui::IsKeyDown(ImGuiKey_ModCtrl) && ImGui::IsKeyPressed(ImGuiKey_Tab, false))
            g_force_tab = (g_tab + (ImGui::IsKeyDown(ImGuiKey_ModShift) ? kTabCount - 1 : 1)) % kTabCount;
    }
    ImVec2 size{std::min(Px(620), io.DisplaySize.x * 0.96f), std::min(Px(540), io.DisplaySize.y * 0.92f)};
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints({Px(380), Px(260)}, io.DisplaySize);
    bool open = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Settings###settings_panel", &open, kFlags)) {
        float last = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##body", {0, -last}, ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("##tabs")) {
            for (int i = 0; i < kTabCount; i++) {
                ImGuiTabItemFlags flags = g_force_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(kTabs[i].name, nullptr, flags)) continue;
                g_tab = i;
                ImGui::BeginChild("##page", {0, 0}, ImGuiChildFlags_NavFlattened);
                if (!kTabs[i].lines) {
                    kTabs[i].draw();
                } else if (BeginLines("##lines")) {
                    kTabs[i].draw();
                    EndLines();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        g_force_tab = -1;
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Close")) open = false;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Esc  close     Ctrl+Tab  next tab     pad: A choose, B close, LB/RB tab");
    }
    ImGui::End();
    if (!open && g_capture < 0 && !g_cooldown) SetOpen(false);

    io.FontGlobalScale = saved_font;
    ImGui::GetStyle() = saved;
}

}  // namespace

void Start() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t dir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    std::filesystem::path font = std::filesystem::path(std::wstring(dir, n)) / L"Fonts" / L"segoeui.ttf";
    std::error_code ec;
    if (n && std::filesystem::exists(font, ec)) g_font = io.Fonts->AddFontFromFileTTF(font.string().c_str(), kFontPx);
    if (!g_font) {
        ImFontConfig c;
        c.SizePixels = kFontPx;
        g_font = io.Fonts->AddFontDefault(&c);
    }
}

void Ui(double fps) {
    if (game::Opt().show_fps) DrawFps(fps);
    DrawNotice();
    bool open = g_open;
    if (open && !g_was_open) Stage();
    g_was_open = open;
    // waiting for a binding the keys are the binding's, not the panel's
    ImGuiIO& io = ImGui::GetIO();
    constexpr int kNav = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (open && g_capture >= 0) {
        if (io.ConfigFlags & kNav) g_nav_flags = io.ConfigFlags & kNav;
        io.ConfigFlags &= ~kNav;
    } else if (g_nav_flags) {
        io.ConfigFlags |= g_nav_flags;
        g_nav_flags = 0;
    }
    if (!open) {
        g_capture = -1;
        g_cooldown = false;
        return;
    }
    Capture();
    Draw();
    // Esc or B closes it, unless a list is open or a text is typed in
    if (g_capture < 0 && !g_cooldown && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        SetOpen(false);
}

bool Message(unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (msg == WM_KEYDOWN && wp == kToggleKey) {
        if (!(lp & (1 << 30))) Toggle();
        *result = 0;
        return true;
    }
    return false;
}

void Toggle() { SetOpen(!g_open); }
void ShowCoop() {
    for (int i = 0; i < kTabCount; i++)
        if (std::strcmp(kTabs[i].name, "Co-op") == 0) g_force_tab = i;
    SetOpen(true);
}
bool Open() { return g_open; }

bool Holds(int port, const input::Pad& pad) {
    if (port == 0 && g_save.exchange(false)) app::Save();
    if (port < 0 || port > 3) return false;
    if (g_open) return true;
    if (!g_hold[port]) return false;
    bool held = pad.buttons != 0 || std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t a) {
                    return a > 0x20;
                });
    for (int vk : {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_LBUTTON, VK_RBUTTON, kToggleKey})
        if (port == 0 && GetAsyncKeyState(vk) < 0) held = true;
    if (!held) g_hold[port] = false;
    return held;
}

}  // namespace panel
