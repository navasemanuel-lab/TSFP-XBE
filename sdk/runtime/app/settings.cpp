// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "net/lockstep.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
// the aspect setting's names and fractions; auto takes the output's
const char* kAspects[] = {"auto", "4:3", "16:9", "16:10", "21:9", "32:9"};
constexpr uint32_t kAspectFractions[][2] = {{0, 0}, {4, 3}, {16, 9}, {16, 10}, {64, 27}, {32, 9}};
constexpr uint32_t kAspectCount = sizeof(kAspects) / sizeof(kAspects[0]);
// the language setting's names, by XC_LANGUAGE (0 unused)
const char* kLanguages[] = {"", "english", "japanese", "german", "french", "spanish", "italian"};
constexpr uint32_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    std::error_code ec;
    s.fresh = !std::filesystem::exists(File(), ec);
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    // at most the output's own: past it the scene is supersampled, at a frame rate few GPUs keep
    s.render_percent = std::clamp<uint32_t>(t["render_percent"].value_or(s.render_percent), 1, 200);
    // a settings file from before the fitting: the scale there is the player's own, kept
    s.render_fitted = t["render_fitted"].value_or(true);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.low_latency = t["low_latency"].value_or(s.low_latency);
    s.seal_seams = t["seal_seams"].value_or(s.seal_seams);
    s.depth_ties = t["depth_ties"].value_or(s.depth_ties);
    s.ao = std::min<uint32_t>(t["ambient_occlusion"].value_or(s.ao), 2);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    // MSAA's samples and the edge smoothing apart; a file from before edge_smoothing has the smoothing in msaa (1
    // FXAA, 2 SMAA, 4 and up the MSAA no longer offered then: FXAA), and no MSAA
    if (auto edge = t["edge_smoothing"].value<uint32_t>()) {
        s.edge_aa = std::min<uint32_t>(*edge, 2);
        uint32_t msaa = t["msaa"].value_or(s.msaa);
        s.msaa = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 0;
    } else {
        uint32_t old = t["msaa"].value_or(0u);
        s.edge_aa = old > 2 ? 1 : old;
        s.msaa = 0;
    }
    s.frame_interp = t["frame_interp"].value_or(s.frame_interp);
    s.fog = t["fog"].value_or(s.fog);
    s.brightness = std::clamp<uint32_t>(t["brightness"].value_or(s.brightness), 50, 150);
    s.hdr = t["hdr"].value_or(s.hdr);
    s.hdr_white = t["hdr_white"].value_or(s.hdr_white);
    if (s.hdr_white) s.hdr_white = std::clamp<uint32_t>(s.hdr_white, 80, 500);
    s.hdr_peak = t["hdr_peak"].value_or(s.hdr_peak);
    if (s.hdr_peak) s.hdr_peak = std::clamp<uint32_t>(s.hdr_peak, 200, 10000);
    std::string aspect = t["aspect_ratio"].value_or(std::string(kAspects[s.aspect]));
    for (uint32_t i = 0; i < kAspectCount; i++)
        if (aspect == kAspects[i]) s.aspect = i;
    std::string language = t["language"].value_or(std::string(kLanguages[s.language]));
    for (uint32_t i = 1; i < kLanguageCount; i++)
        if (language == kLanguages[i]) s.language = i;
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    std::string link = t["link"].value_or(std::string(s.link_mode == 1 ? "host" : s.link_mode == 2 ? "join" : "off"));
    s.link_mode = link == "host" ? 1 : link == "join" ? 2 : 0;
    s.link_address = t["link_address"].value_or(s.link_address);
    s.link_port = std::clamp<uint32_t>(t["link_port"].value_or(s.link_port), 1, 65535);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"render_fitted", s.render_fitted},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"low_latency", s.low_latency},
                  {"seal_seams", s.seal_seams},
                  {"depth_ties", s.depth_ties},
                  {"ambient_occlusion", int64_t(s.ao)},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", int64_t(s.msaa)},
                  {"edge_smoothing", int64_t(s.edge_aa)},
                  {"brightness", int64_t(s.brightness)},
                  {"hdr", s.hdr},
                  {"hdr_white", int64_t(s.hdr_white)},
                  {"hdr_peak", int64_t(s.hdr_peak)},
                  {"frame_interp", int64_t(s.frame_interp)},
                  {"fog", s.fog},
                  {"aspect_ratio", kAspects[std::min(s.aspect, kAspectCount - 1)]},
                  {"language", kLanguages[std::clamp<uint32_t>(s.language, 1, kLanguageCount - 1)]},
                  {"join_ip", s.join_ip},
                  {"link", s.link_mode == 1 ? "host" : s.link_mode == 2 ? "join" : "off"},
                  {"link_address", s.link_address},
                  {"link_port", int64_t(s.link_port)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

// the settings as last logged or saved: a save logs the lines that changed since, so a performance log shows what
// each stretch of it was played with
std::string g_logged;

std::string Text() {
    std::ostringstream text;
    text << Table();
    return text.str();
}

}  // namespace

void Save() {
    // a co-op session plays on some of the host's settings (net/lockstep.h): this machine's file keeps its own
    if (lockstep::Active()) return;
    std::string text = Text();
    std::ofstream f(File());
    f << text << "\n";
    if (!g_logged.empty() && text != g_logged) {
        std::set<std::string> before;
        std::istringstream old(g_logged), now(text);
        for (std::string line; std::getline(old, line);) before.insert(line);
        std::string changed;
        for (std::string line; std::getline(now, line);)
            if (!line.empty() && line[0] != '[' && !before.count(line)) changed += (changed.empty() ? "" : ", ") + line;
        if (!changed.empty()) LOGR("settings: %s", changed.c_str());
    }
    g_logged = text;
}

void LogSettings() {
    g_logged = Text();
    std::istringstream lines(g_logged);
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

void AspectFraction(uint32_t& w, uint32_t& h) {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<uint64_t> fraction{uint64_t(16) << 32 | 9};
    const Settings& s = Get();
    // a co-op session: 16:9 on both machines, the game sizes its views by the shape
    if (lockstep::Active()) {
        w = 16, h = 9;
        return;
    }
    if (s.aspect && s.aspect < kAspectCount) {
        w = kAspectFractions[s.aspect][0];
        h = kAspectFractions[s.aspect][1];
        return;
    }
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        uint32_t a = std::max(m.width, 1u), b = std::max(m.height, 1u);
        // narrower than 4:3: the 4:3 picture, bars above and below
        if (uint64_t(a) * 3 < uint64_t(b) * 4) a = 4, b = 3;
        uint32_t x = a, y = b;
        while (y) {
            x %= y;
            std::swap(x, y);
        }
        fraction = uint64_t(a / x) << 32 | (b / x);
    }
    uint64_t f = fraction;
    w = uint32_t(f >> 32);
    h = uint32_t(f);
}

float Aspect() {
    uint32_t w, h;
    AspectFraction(w, h);
    return float(w) / float(h);
}

bool Widescreen() { return Aspect() > 4.0f / 3.0f + 0.01f; }

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes (after TGP482/XboxXBE-Recomp 16bf1cd)
Mode CachedOutput() {
    static std::mutex lock;
    static uint32_t width = 0, height = 0;
    static HMONITOR monitor = nullptr;
    static Mode mode{};
    const Settings& s = Get();
    HMONITOR now = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    std::lock_guard<std::mutex> guard(lock);
    if (s.width != width || s.height != height || now != monitor || !mode.height) {
        width = s.width, height = s.height, monitor = now;
        mode = Output();
    }
    return mode;
}

Scale2 RenderScale(uint32_t guest_width, uint32_t guest_height) {
    if (!guest_width || !guest_height) return {1.0f, 1.0f};
    // the picture as the output shows it, bars left out
    Mode m = CachedOutput();
    float aspect = Aspect();
    float w = std::min(float(m.width), m.height * aspect);
    // over 100% the scene is drawn larger than the output and shrunk to it, at most 7680 wide (8K)
    float k = std::min(Get().render_percent / 100.0f, 7680.0f / w);
    return {std::max(w * k / guest_width, 0.1f), std::max(w / aspect * k / guest_height, 0.1f)};
}

}  // namespace app
