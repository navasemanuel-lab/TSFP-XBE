// Online Story co-op (net/lockstep.h): two PCs run the same split screen co-op of two players, frame for frame, and
// each shows its own player full screen. After lmplied/TSFPRecomp's lockstep (docs/technical/lockstep.md), whose
// findings on this game these are; the code is this port's own.
//
// The frame: the main loop (0x3D390) draws the frame (0x3C440), then steps the frame clock (0x1538C0), which waits for
// the vertical blank count (0x22030 reads [0x563918], counted by the blank callback 0x22020) to move on from the last
// frame's and steps the world by the blanks counted, 1 to 3. In a session the clock's read from there is "one more than
// the last", so the world steps one blank a frame on both machines; the session's frame starts there (FrameStart: the
// controls of both players, the pacing to 60 a second, the clocks and the sound). The frame clock keeps the count in
// [0x7497A8], the game's time stamp (some 80 readers). The loading screen's thread (0x156CB0) reads the real count
// still and shows the screen once a blank while a level loads, but it also counted [0x7497A8] on, as many times as the
// load took on this machine, and the level's setup reads it (0x1286D0): in a session its loop runs here without that.
//
// The views: the screen's layout (0x25A570, per player through 0x55D90) is the full screen for every player on both
// machines (a layout of one screen while it runs: the screen count 0x1CDAE0 answers 1), so both games size things
// alike. The frame then draws each player's world (0xC8120, from 0x3C794), each player's HUD (0xC84A0, from 0x3C842)
// and each player's overlays (0xC8500, from 0x3C893), the player at [0x7B0C7C] (+4 the input slot): the other
// player's passes are drawn into nothing (d3d::g_hide_draws). The screen count answers 1 in the passes too: the HUD
// sizes itself by it (the weapon bar's guns at 0x143D20 are put twice as near the camera for two screens, 0x141590
// moves things for a half screen), and a full screen view wants one screen's. Both machines run every pass alike, so
// they still agree. Cutscenes draw from player 1's world pass alone: when
// this machine's pass drew almost nothing last frame and the other's did, the other's is shown. The 2D layer (menus,
// boxes, sprites) goes on a list drawn when it is flushed (0x22FF0), which the overlay pass leaves for later: each pass
// flushes it before and after, so one player's pause menu is not drawn on the other's screen nor its boxes lost. This
// machine's passes are drawn at its own field of view (fov.cpp), the camera put back after.
//
// The sounds: both games play every sound alike, the other player's pause menu's too. While it is up, the sounds its
// menus start (0x21780, a 2D sound by its id) play silent here: the game still starts them, so both games agree.
//
// A session is Story's alone: a level of another game asked for (0x658D0) is refused, on both machines alike, and the
// menus stay where they are.
//
// The rest of the session's needs live where they are met: the mouse's turns (aim.cpp), the pointer and typing off
// (mouse.cpp), the screen's shape (app::AspectFraction), the settings pinned to the host's (game_info.cpp).
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "audio/audio.h"
#include "common/controls.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace game {
bool TakeMouseTurn(float& dx, float& dy);  // aim.cpp
void LocalView(bool on);                   // fov.cpp
void NetBotsConfig(uint32_t config);       // net_bots.cpp
bool StoryOnlinePrepare(X86Context& ctx, uint32_t address);  // story_online.cpp
void StoryOnlineActivate();
}

namespace {

constexpr uint32_t kPlayer = 0x7B0C7C;  // the player being drawn or updated
constexpr uint32_t kSlot = 0x4;         // its input slot
constexpr uint32_t kFrameClockRead = 0x153975;  // 0x1538C0's call of 0x22030 returns here
constexpr uint32_t kBlankCount = 0x563918;      // the vertical blank count 0x22030 reads
constexpr uint32_t kLoadScreenRead = 0x156CB6;  // and the loading screen thread's (0x156CB0)
constexpr uint32_t kWorldPass = 0x3C799, kHudPass = 0x3C847, kOverlayPass = 0x3C898;  // the per-player calls' returns
constexpr uint32_t kFewDraws = 50;
constexpr uint32_t kFlush2D = 0x22FF0;  // the 2D list drawn
constexpr uint32_t kPauseState = 0x761C48;  // +0 bit 0 a pause menu up
constexpr uint32_t kPauseRoot = 0x761BBC;   // the pause screen's menu: +0x38 the slot of the player who paused

constexpr uint32_t kScenes = 100;  // a level's scene is up to 100, over it the front end
constexpr uint8_t kStory = 10;

std::atomic<int64_t> g_refused_at{0};  // when a game other than Story was refused, ms of the steady clock

uint32_t g_blanks = 0;                  // the frame clock's count in a session
thread_local bool t_one_screen = false;  // the layout is being made, or a per-player pass drawn

// this machine's player's controls: the keyboard, the mouse's buttons and every controller, one player
input::Pad Capture(float& turn_x, float& turn_y, bool& keyboard) {
    // both games on this PC: the host has the keyboard and mouse, player 2 the controllers
    bool shared = lockstep::PeerOnThisPC(), pads = !shared || lockstep::LocalPort() == 1;
    turn_x = turn_y = 0;
    keyboard = false;
    input::Pad p = shared && pads ? input::Pad{} : input::PollLocal(0);
    for (int port = shared ? 0 : 1; pads && port < 4; port++) {
        if (shared && port == 0 && game::Opt().keyboard_own_port) continue;
        input::Pad c = input::PollLocal(port);
        if (!c.connected) continue;
        p.buttons |= c.buttons;
        for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], c.analog[i]);
        auto take = [](int16_t& mine, int16_t theirs) {
            if (std::abs(int(theirs)) > std::abs(int(mine))) mine = theirs;
        };
        take(p.lx, c.lx), take(p.ly, c.ly), take(p.rx, c.rx), take(p.ry, c.ry);
    }
    p.connected = true;
    if (shared && pads) return p;
    if (!game::TakeMouseTurn(turn_x, turn_y)) turn_x = turn_y = 0;
    keyboard = controls::KeyboardActive();
    return p;
}

// a per-player pass is drawn here when its player is this machine's
struct Pass {
    uint32_t draws[2] = {};  // the world passes' draws last frame: this machine's player's, the other's
    uint32_t counting[2] = {};
};
Pass g_pass;

bool IsLocal(uint32_t player) {
    return player && mem::IsCommitted(player) && kernel::R32(player + kSlot) == uint32_t(lockstep::LocalPort());
}

// runs a per-player pass, drawn into nothing when it is the other player's
template <typename Original>
void RunPass(X86Context& ctx, uint8_t* base, Original original, bool world) {
    uint32_t player = kernel::R32(kPlayer);
    bool local = IsLocal(player);
    // a cutscene draws from one player's pass: shown here if this machine's own drew almost nothing last frame
    bool show = local || (g_pass.draws[0] < kFewDraws && g_pass.draws[1] >= kFewDraws);
    uint32_t before = d3d::g_draw_count;
    auto flush = [&] {
        X86Context saved = ctx;
        kernel::CallGuest(ctx, kFlush2D, {});
        ctx = saved;
    };
    flush();  // the 2D queued before, drawn as it was
    bool was = d3d::g_hide_draws.exchange(!show);
    bool one = std::exchange(t_one_screen, true);
    if (local) game::LocalView(true);
    original(ctx, base);
    flush();  // this pass's 2D, drawn or hidden with it
    if (local) game::LocalView(false);
    t_one_screen = one;
    d3d::g_hide_draws = was;
    if (world) g_pass.counting[local ? 0 : 1] += d3d::g_draw_count - before;
}

// the slot of the player whose pause menu is up, -1 when none or unknown
int PausedBy() {
    uint32_t pause = kernel::R32(kPauseState);
    if (pause < 0x10000 || !mem::IsCommitted(pause) || !(kernel::R32(pause) & 1)) return -1;
    uint32_t menu = kernel::R32(kPauseRoot);
    if (menu < 0x10000 || !mem::IsCommitted(menu + 0x3B)) return -1;
    uint32_t slot = kernel::R32(menu + 0x38);
    return slot < 4 ? int(slot) : -1;
}

}  // namespace

namespace game {

bool g_in_movie = false;  // PlayMovie runs (movies.cpp): its loop shows a frame a Swap

// game_info.cpp Init: what a session needs of the game, before it is set up
void StartCoopOnline() {
    lockstep::SetCapture(Capture);
    // a movie's loop steps no frame clock: each frame it shows is a session's frame
    d3d::g_on_swap = [] {
        if (g_in_movie && lockstep::Active()) lockstep::FrameStart();
    };
    // compared every second: the game's globals (the players, the level, the AI's own), without what differs
    // between machines and does not matter
    lockstep::HashRange(0x4B82C0, 0x333594);  // .data
    lockstep::HashSkip(0x54DB04, 4);          // the stack cookie, seeded from the time at start
    lockstep::HashSkip(0x7A58A0, 0x60);       // the frame clock's profiling: rdtsc and its history
    lockstep::HashSkip(0x563918, 4);          // the vertical blank count, the real one
    lockstep::HashSkip(0x7497A8, 4);          // the blank count's copy, the loading screen counts it on
    lockstep::HashSkip(0x7497D8, 4);
    lockstep::HashSkip(0x749848, 4);          // the loading bar
    // the file streamer (music, speech): its cache of three 512 KB blocks and their slots (0x54F128), the block being
    // read and how far (0x4B85D0, 0x4B90A0, 0x55F980): where it is follows how fast this machine's disk reads, a block
    // ahead on one or the other
    lockstep::HashSkip(0x54F128, 0x840);
    lockstep::HashSkip(0x4B85D0, 0x18);
    lockstep::HashSkip(0x4B90A0, 0x10);
    lockstep::HashSkip(0x55F980, 0x10);
    lockstep::HashSkip(0x55F974, 4);   // a draw list's write pointer, the loading screen's drawing goes on it
    lockstep::HashSkip(0x74A9B4, 4);   // the loading screen's spinning angle
}

}  // namespace game

// LoadingScreenThread(): in a session its loop without the count of [0x7497A8]
constexpr uint32_t kLastBlank = 0x7497D8, kLoadEvent = 0x749888, kLoadDone = 0x74A9B0, kLoadThread = 0x7497E0;
X86_EXTERN_FUNC(__imp__sub_00156CB0);
X86_FUNC(sub_00156CB0) {
    if (!lockstep::Active()) {
        __imp__sub_00156CB0(ctx, base);
        return;
    }
    for (;;) {
        uint32_t blank;
        while ((blank = kernel::CallGuest(ctx, 0x22030, {})) == kernel::R32(kLastBlank)) std::this_thread::yield();
        kernel::W32(kLastBlank, blank);
        kernel::CallGuest(ctx, 0x156890, {});  // the screen
        kernel::CallGuest(ctx, 0x380029, {kernel::R32(kLoadEvent), 8, 0});
        if (kernel::R32(kLoadDone)) break;
    }
    // the thread's end, as the loop's: its exit code, then ExitThread
    ctx.esp -= 4;
    uint32_t code = ctx.esp;
    kernel::CallGuest(ctx, 0x37FD83, {kernel::R32(kLoadThread), code});
    kernel::CallGuest(ctx, 0x37FD71, {kernel::R32(code)});
}

// ReadBlankCount(): the frame clock's read is one more than the last in a session. Outside one the frame clock and the
// loading screen's thread read the count until it moves on; a read that finds it where the last one left it sleeps
// until the next blank instead, rather than spinning a core: between frames, and during a load beside the thread
// loading the level
X86_EXTERN_FUNC(__imp__sub_00022030);
X86_FUNC(sub_00022030) {
    uint32_t caller = kernel::R32(ctx.esp);
    if (caller == kFrameClockRead && lockstep::Active()) {
        kernel::Return(ctx, ++g_blanks, 0);
        return;
    }
    static uint32_t last = 0;
    // the title's count (its callback's) is not the device's the blank thread wakes on
    if (caller == kFrameClockRead) {
        uint32_t seen = d3d::BlankSeen();
        if (kernel::R32(kBlankCount) == last) d3d::WaitBlank(seen);
        __imp__sub_00022030(ctx, base);
        last = ctx.eax;
        return;
    }
    if (caller == kLoadScreenRead) {
        uint32_t seen = d3d::BlankSeen();
        if (kernel::R32(kBlankCount) == kernel::R32(kLastBlank)) d3d::WaitBlank(seen);
    }
    __imp__sub_00022030(ctx, base);
}

namespace game {
void NetDiagFrame();  // game/net_diag.cpp
}

// FrameClock(): a session's frame starts (and the network's diagnostics look)
X86_EXTERN_FUNC(__imp__sub_001538C0);
X86_FUNC(sub_001538C0) {
    game::NetDiagFrame();
    if (lockstep::Active()) {
        lockstep::FrameStart();
        g_pass.draws[0] = g_pass.counting[0];
        g_pass.draws[1] = g_pass.counting[1];
        g_pass.counting[0] = g_pass.counting[1] = 0;
    }
    __imp__sub_001538C0(ctx, base);
}

// LayOutView(player, camera): every player's view the full screen in a session
X86_EXTERN_FUNC(__imp__sub_0025A570);
X86_FUNC(sub_0025A570) {
    if (!lockstep::Active()) {
        __imp__sub_0025A570(ctx, base);
        return;
    }
    bool one = std::exchange(t_one_screen, true);
    __imp__sub_0025A570(ctx, base);
    t_one_screen = one;
}

// ScreenCount(): one while a session's layout is made or a per-player pass drawn
X86_EXTERN_FUNC(__imp__sub_001CDAE0);
X86_FUNC(sub_001CDAE0) {
    if (t_one_screen) {
        kernel::Return(ctx, 1, 0);
        return;
    }
    __imp__sub_001CDAE0(ctx, base);
}

// PlaySound2D(id): silent here while the other player's pause menu is up, whose sounds these are
X86_EXTERN_FUNC(__imp__sub_00021780);
X86_FUNC(sub_00021780) {
    int paused = lockstep::Active() ? PausedBy() : -1;
    if (paused < 0 || paused == lockstep::LocalPort()) {
        __imp__sub_00021780(ctx, base);
        return;
    }
    audio::MutePlays(true);
    __imp__sub_00021780(ctx, base);
    audio::MutePlays(false);
}

// DrawWorld(), DrawHud(), DrawOverlays(): the per-player passes
X86_EXTERN_FUNC(__imp__sub_000C8120);
X86_FUNC(sub_000C8120) {
    if (lockstep::Active() && kernel::R32(ctx.esp) == kWorldPass) RunPass(ctx, base, __imp__sub_000C8120, true);
    else __imp__sub_000C8120(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000C84A0);
X86_FUNC(sub_000C84A0) {
    if (lockstep::Active() && kernel::R32(ctx.esp) == kHudPass) RunPass(ctx, base, __imp__sub_000C84A0, false);
    else __imp__sub_000C84A0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000C8500);
X86_FUNC(sub_000C8500) {
    if (lockstep::Active() && kernel::R32(ctx.esp) == kOverlayPass) RunPass(ctx, base, __imp__sub_000C8500, false);
    else __imp__sub_000C8500(ctx, base);
}

namespace game {
// a line for the screen in a session: a few seconds after a game other than Story was asked for, or while the other
// player's pause menu is up
std::string CoopNotice() {
    using namespace std::chrono;
    int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t at = g_refused_at.load();
    if (!lockstep::Active()) return {};
    if (at && now - at <= 4000) return "Online co-op plays Story only: choose Story, then 2 players.";
    int paused = PausedBy();
    if (paused >= 0 && paused != lockstep::LocalPort())
        return "Player " + std::to_string(paused + 1) + " paused the game.";
    return {};
}
}  // namespace game

// GoToLevel(config): the level and game to load (+0x54 the scene, +0x55 the game), Story's alone in a session; a
// System Link host's bots put in first
X86_EXTERN_FUNC(__imp__sub_000658D0);
X86_FUNC(sub_000658D0) {
    if (lockstep::Active()) {
        uint32_t config = kernel::R32(ctx.esp + 4);
        uint32_t scene = kernel::R32(config + 0x54) & 0xFF, game = (kernel::R32(config + 0x54) >> 8) & 0xFF;
        if (scene <= kScenes && game != kStory) {
            using namespace std::chrono;
            g_refused_at = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            LOGR("coop: scene %u of game %u refused, a session is Story's", scene, game);
            kernel::Return(ctx, 0, 0);
            return;
        }
    }
    game::NetBotsConfig(kernel::R32(ctx.esp + 4));
    bool story = game::StoryOnlinePrepare(ctx, kernel::R32(ctx.esp + 4));
    __imp__sub_000658D0(ctx, base);
    if (story) game::StoryOnlineActivate();
}
