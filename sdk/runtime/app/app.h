#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height, 1 to 200
    uint32_t fps_cap = 60;           // 0 unlimited
    bool vsync = true;
    bool low_latency = true;         // one frame queued for the display, not two: shown a frame sooner after the input
    uint32_t aniso = 0;              // 0 as the game asks
    uint32_t aspect = 0;             // the frame's shape: 0 the output's, 1 4:3, 2 16:9, 3 16:10, 4 21:9, 5 32:9
    uint32_t msaa = 0;               // the scene's samples a pixel: 0 off, 2, 4 or 8
    uint32_t edge_aa = 0;            // the final picture's edges smoothed: 0 off, 1 FXAA, 2 SMAA (with MSAA or not)
    uint32_t ao = 0;  // ambient occlusion: 0 off, 1 SSAO, 2 GTAO (soft shadows where surfaces meet)
    bool depth_ties = true;  // depth rounded to the console's units: near-coplanar surfaces tie as there, no flicker
    bool seal_seams = true;          // conservative rasterization of solid geometry: no see-through cracks between pieces
    uint32_t frame_interp = 1;       // frames shown for each the title draws: 1, or 2 to 4 with frames drawn between
    // set at start from the GPU, not saved: on an integrated one the scene's pixel math at reduced precision, no MSAA,
    // anisotropic filtering at most 4x and the picture scaled to the output plainly
    bool low_profile = false;
    // the console's language (XC_LANGUAGE) the title reads at start: 1 English, 2 Japanese, 3 German, 4 French,
    // 5 Spanish, 6 Italian
    uint32_t language = 1;
    bool fog = true;                 // the scene's fog; off it is drawn clear to the far plane
    // the picture's brightness, a percent: over 100 the darks and middle tones lifted, under it lowered, white and
    // black kept
    uint32_t brightness = 100;
    // the render scale has been fitted to the GPU's speed in play (FitRenderScale): no longer lowered by itself
    bool render_fitted = false;
    bool fresh = false;              // no settings file yet: the renderer fits the render scale to the GPU
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // System Link past the local network, through one player's game (xdk/xnet.cpp): 0 off (the local network and
    // VPNs), 1 this game hosts, 2 it joins link_address (the host's address, :port optional); on link_port
    uint32_t link_mode = 0;
    std::string link_address;
    uint32_t link_port = 7778;
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// host pixels per guest pixel along each axis: the guest frame drawn at the size the output shows it, times
// render_percent
struct Scale2 {
    float x, y;
};
Scale2 RenderScale(uint32_t guest_width, uint32_t guest_height);
// the shown picture's shape, from the aspect setting or on auto the output's (4:3 at the narrowest), as a fraction in
// whole numbers and as its ratio
void AspectFraction(uint32_t& w, uint32_t& h);
float Aspect();
// wider than 4:3: the console reports a widescreen TV, else a 4:3 one
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    const wchar_t* launcher = nullptr;  // beside the exe, opened instead of the missing message to bring the game in
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();
// sleeps until `at` on a high resolution waitable timer, without spinning a core
void SleepUntil(std::chrono::steady_clock::time_point at);

}  // namespace app
