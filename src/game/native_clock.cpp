// Native frame rate: the game steps its own update and draw loop as often as the display (or the chosen rate) allows,
// each frame with its real, fractional span of the console's 60 Hz ticks, instead of one whole tick per vertical blank.
//
// The game keeps time in two forms: whole ticks (0x4E7940 raw, 0x4E7944 game, which a pause zeroes) for counters, and
// the same span as a float (0x4E7948, 0x4E794C) for motion. Here the ticks carry their remainder from frame to frame
// (a counter still reaches one on the right frame, so it runs at 60 Hz whatever the frame rate) and the floats are the
// frame's fraction. The loads the game does from the tick words are then fixed up by hooks (config/game.toml's native
// frame rate block, tools/native_hooks.py): a conversion of a tick word to a float takes the float beside it, and the
// few "did time pass" gates, which would skip a whole update on a frame with no whole tick, ask the float.
//
// Ideas and the gate list are from the native high refresh work of TheDoffman's timesplitters-future-perfect-pc (GPLv3).
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
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(sub_000455B0);  // a pause begins
X86_EXTERN_FUNC(sub_000455C0);  // a pause ends

namespace game {
namespace {

constexpr uint32_t kRawTicks = 0x4E7940, kGameTicks = 0x4E7944;  // whole ticks of this frame
constexpr uint32_t kRawDelta = 0x4E7948, kGameDelta = 0x4E794C;  // and as a float
constexpr uint32_t kTotalTicks = 0x7497B0, kFrameCount = 0x7497B8, kCatchUps = 0x7497BC;
constexpr uint32_t kPause = 0x7497B4;                             // 0 playing, >0 paused, -1 a pause just ended
constexpr uint32_t kBlankCount = 0x563918;
constexpr uint32_t kWaitedBlank = 0x7497A8, kLastBlank = 0x7497AC, kClockBlank = 0x7A58B0;
constexpr uint32_t kHistory = 0x7A58A0;                           // the last frames' blank counts (4), deltas (0x20, 0x40 on)
constexpr uint32_t kFrameTime = 0x7A58F0, kRefresh = 0x4E8CEC;
constexpr uint32_t kUptime = 0x74914C;                            // 0x150190's clock: ticks, seconds, minutes, hours

std::atomic<bool> g_active{false};

float F32(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void SetF32(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }

using clock = std::chrono::steady_clock;

// the rate asked for: 0 the game's own 60, 1 the display's, else that many a second, 1000 and over without a limit.
// XBE_FPS overrides the setting
uint32_t Target() {
    static const int64_t env = [] {
        const char* e = std::getenv("XBE_FPS");
        return e ? int64_t(std::atoi(e)) : int64_t(-1);
    }();
    return env >= 0 ? uint32_t(env) : Opt().fps_target;
}

// the refresh rate of the monitor the game shows on
uint32_t DisplayHz() {
    static uint32_t hz = 0;
    static clock::time_point at;
    if (hz && clock::now() - at < std::chrono::seconds(5)) return hz;
    at = clock::now();
    hz = 60;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(static_cast<HMONITOR>(app::TargetMonitor()), &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            hz = dm.dmDisplayFrequency;
    }
    return hz;
}

// the time between frames in 60 Hz ticks, the whole ticks carried: nanoseconds * 60 over 1e9 per tick
struct Timing {
    int64_t last = 0, raw_rest = 0, game_rest = 0;
    bool started = false;
};
struct Step {
    float raw_delta = 0, game_delta = 0;
    uint32_t raw_ticks = 0, game_ticks = 0;
};

Step StepFor(Timing& t, int64_t now, bool paused) {
    constexpr int64_t kSecond = 1000000000;
    Step s;
    if (!t.started) {
        t.started = true;
        t.last = now;
        t.raw_rest = t.game_rest = 0;
        return s;
    }
    int64_t elapsed = std::max<int64_t>(0, now - t.last);
    t.last = std::max(t.last, now);
    // like the game's own three tick limit: a load or a stop in a debugger is not one huge step
    elapsed = std::min<int64_t>(elapsed, kSecond / 20);
    int64_t scaled = elapsed * 60;
    s.raw_delta = float(double(scaled) / double(kSecond));
    t.raw_rest += scaled;
    s.raw_ticks = uint32_t(t.raw_rest / kSecond);
    t.raw_rest %= kSecond;
    if (!paused) {
        s.game_delta = s.raw_delta;
        t.game_rest += scaled;
        s.game_ticks = uint32_t(t.game_rest / kSecond);
        t.game_rest %= kSecond;
    }
    return s;
}

// waits for the frame's slot on a schedule that keeps its beat: a frame that ran late does not make the next hurry
struct Pacer {
    clock::time_point epoch;
    int64_t slot = 0;
    uint32_t rate = 0;
};

void Wait(Pacer& p, uint32_t rate) {
    auto now = clock::now();
    if (p.rate != rate) {
        p.rate = rate;
        p.epoch = now;
        p.slot = 0;
    }
    ++p.slot;
    auto due = p.epoch + std::chrono::nanoseconds(p.slot * 1000000000ll / rate);
    if (now < due) {
        app::SleepUntil(due);
    } else if (now - due > std::chrono::milliseconds(100)) {
        p.epoch = now;  // far behind (a load): start the schedule again, no burst of frames
        p.slot = 0;
    } else if (now > due) {
        // a missed slot is dropped, not made up
        int64_t late = std::chrono::duration_cast<std::chrono::nanoseconds>(now - p.epoch).count() * rate / 1000000000ll;
        if (late > p.slot) p.slot = late;
    }
}

// 0x150190: the uptime counters the clock feeds
void Uptime() {
    int32_t ticks = int32_t(kernel::R32(kUptime) + kernel::R32(kRawTicks));
    int32_t per_second = int32_t(kernel::R32(kRefresh));
    kernel::W32(kUptime, uint32_t(ticks));
    if (ticks < per_second) return;
    kernel::W32(kUptime, uint32_t(ticks - per_second));
    uint32_t sec = kernel::R32(0x749148) + 1;
    kernel::W32(0x749148, sec);
    if (sec != 60) return;
    uint32_t min = kernel::R32(0x749158) + 1;
    kernel::W32(0x749148, 0);
    kernel::W32(0x749158, min);
    if (min != 60) return;
    uint32_t hour = kernel::R32(0x749160) + 1;
    kernel::W32(0x749158, 0);
    kernel::W32(0x749160, hour);
    if (hour == 24) kernel::W32(0x749160, 0);
}

}  // namespace

bool NativeFrameRate() { return g_active.load(std::memory_order_relaxed); }

// FrameClock(), the frame's end: false leaves it to the game's own
bool NativeFrameClock(X86Context& ctx, uint8_t* base) {
    static Timing timing;
    static Pacer pacer;
    static bool announced = false;
    uint32_t target = Target();
    uint32_t rate = target == 1 ? DisplayHz() : target;
    if (!target || rate < 2) {
        if (g_active.exchange(false)) {
            // back on the game's clock: its wait is relative to the last blank it saw
            uint32_t blank = kernel::R32(kBlankCount);
            kernel::W32(kWaitedBlank, blank);
            kernel::W32(kLastBlank, blank);
            kernel::W32(kClockBlank, blank);
            timing = Timing{};
            announced = false;
            app::g_external_pace = false;
            LOGR("fps: the game's own 60 Hz clock");
        }
        return false;
    }
    if (!g_active.exchange(true)) {
        app::g_external_pace = true;
        timing = Timing{};
        pacer = Pacer{};
    }
    // the display paces a frame by itself when it shows each one (vertical sync): the display's rate needs no timer
    bool paced = !(target == 1 && app::Get().vsync) && target < 1000;
    if (paced) Wait(pacer, rate);
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    int32_t pause = int32_t(kernel::R32(kPause));
    Step s = StepFor(timing, now, pause > 0);
    if (!announced) {
        announced = true;
        LOGR("fps: native frames, %s%u a second, fractional ticks", target == 1 ? "the display's " : target >= 1000 ? "no limit, " : "", rate);
    }
    // the three histories the game's clock keeps of the last frames
    for (uint32_t i = 0; i < 4; i++) {
        kernel::W32(kHistory + i * 4, kernel::R32(kHistory + 4 + i * 4));
        kernel::W32(kHistory + 0x20 + i * 4, kernel::R32(kHistory + 0x24 + i * 4));
        kernel::W32(kHistory + 0x40 + i * 4, kernel::R32(kHistory + 0x44 + i * 4));
    }
    uint32_t blank = kernel::R32(kBlankCount);
    kernel::W32(kWaitedBlank, blank);
    kernel::W32(kLastBlank, blank);
    kernel::W32(kClockBlank, blank);
    if (uint32_t refresh = kernel::R32(kRefresh))
        SetF32(kFrameTime, float(uint32_t(x86_rdtsc())) / float(733333333u / refresh));
    kernel::W32(kRawTicks, s.raw_ticks);
    SetF32(kRawDelta, s.raw_delta);
    kernel::W32(kFrameCount, kernel::R32(kFrameCount) + 1);
    if (s.raw_ticks > 1) kernel::W32(kCatchUps, kernel::R32(kCatchUps) + 1);
    // the first frame of a pause: the game is told while its float delta still shows the time running
    if (pause > 0 && F32(kGameDelta) > 0) {
        X86_PUSH32(0x153A45u);
        sub_000455B0(ctx, base);
    }
    kernel::W32(kGameTicks, s.game_ticks);
    SetF32(kGameDelta, s.game_delta);
    if (pause < 0) {
        kernel::W32(kPause, 0);
        X86_PUSH32(0x153AABu);
        sub_000455C0(ctx, base);
    }
    kernel::W32(kTotalTicks, kernel::R32(kTotalTicks) + s.raw_ticks);
    Uptime();
    ctx.esp += 4;  // the return
    return true;
}

}  // namespace game

// FrameClock's tick words as the game reads them, hooks of config/game.toml (tools/native_hooks.py). The conversions of
// a tick word to a float take the float beside it (+8): the frame's real span, where the whole ticks carry a remainder
#define NATIVE_XMM(n)                                                                       \
    void NativeGameXmm##n(X86Context& ctx, uint8_t*) {                                      \
        if (game::NativeFrameRate()) ctx.xmm[n].f[0] = std::bit_cast<float>(kernel::R32(0x4E794C)); \
    }                                                                                       \
    void NativeRawXmm##n(X86Context& ctx, uint8_t*) {                                       \
        if (game::NativeFrameRate()) ctx.xmm[n].f[0] = std::bit_cast<float>(kernel::R32(0x4E7948)); \
    }
NATIVE_XMM(0)
NATIVE_XMM(1)
NATIVE_XMM(2)
NATIVE_XMM(3)
NATIVE_XMM(4)
NATIVE_XMM(5)
NATIVE_XMM(6)
NATIVE_XMM(7)
#undef NATIVE_XMM

// fild of a tick word: st(0) is the loaded integer
void NativeGameFild(X86Context& ctx, uint8_t*) {
    if (game::NativeFrameRate()) X87_ST(0) = double(std::bit_cast<float>(kernel::R32(0x4E794C)));
}
void NativeRawFild(X86Context& ctx, uint8_t*) {
    if (game::NativeFrameRate()) X87_ST(0) = double(std::bit_cast<float>(kernel::R32(0x4E7948)));
}

// the loads that only ask whether game time passed: the register is tested or replaced right after
static uint32_t TimePassed() {
    return std::bit_cast<float>(kernel::R32(0x4E794C)) > 0.0f ? 1u : 0u;
}
void NativeGateAx(X86Context& ctx, uint8_t*) { if (game::NativeFrameRate()) ctx.eax = TimePassed(); }
void NativeGateCx(X86Context& ctx, uint8_t*) { if (game::NativeFrameRate()) ctx.ecx = TimePassed(); }
void NativeGateDx(X86Context& ctx, uint8_t*) { if (game::NativeFrameRate()) ctx.edx = TimePassed(); }
void NativeGateDi(X86Context& ctx, uint8_t*) { if (game::NativeFrameRate()) ctx.edi = TimePassed(); }

// the pad poll (sub_0018FF70(record, port)): the front end takes a button's press and release on whole ticks. On a frame
// with none the poll still reads the sticks, but the buttons keep the state they had, so the next whole tick sees
// every change once
X86_EXTERN_FUNC(__imp__sub_0018FF70);
X86_FUNC(sub_0018FF70) {
    if (!game::NativeFrameRate() || kernel::R32(0x4E7940)) {
        __imp__sub_0018FF70(ctx, base);
        return;
    }
    uint32_t record = kernel::R32(ctx.esp + 4);
    if (!record) {
        ctx.esp += 4;
        return;
    }
    uint32_t held = kernel::R32(record + 12);
    __imp__sub_0018FF70(ctx, base);
    kernel::W32(record + 8, held);
    kernel::W32(record + 12, held);
    kernel::W32(record + 16, 0);
    kernel::W32(record + 20, 0);
}
