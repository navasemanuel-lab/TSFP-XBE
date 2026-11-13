// The game's frame clock. The main loop (0x3D390) draws the frame (0x3C440), then steps the frame clock (0x1538C0), which
// waits for the vertical blank count (0x22030 reads [0x563918], counted by the blank callback 0x22020) to move on from
// the last frame's. The loading screen's thread (0x156CB0) reads it too.
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace game {
void NetBotsConfig(uint32_t config);  // net_bots.cpp
void NetDiagFrame();                  // net_diag.cpp
bool NativeFrameClock(X86Context& ctx, uint8_t* base);  // native_clock.cpp
}

constexpr uint32_t kFrameClockRead = 0x153975;  // 0x1538C0's call of 0x22030 returns here
constexpr uint32_t kBlankCount = 0x563918;      // the vertical blank count 0x22030 reads
constexpr uint32_t kLoadScreenRead = 0x156CB6;  // and the loading screen thread's (0x156CB0)
constexpr uint32_t kLastBlank = 0x7497D8;       // the loading screen thread's last

// ReadBlankCount(): the frame clock and the loading screen's thread read the count until it moves on; a read that finds
// it where the last one left it sleeps until the next blank instead, rather than spinning a core: between frames, and
// during a load beside the thread loading the level
X86_EXTERN_FUNC(__imp__sub_00022030);
X86_FUNC(sub_00022030) {
    uint32_t caller = kernel::R32(ctx.esp);
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

// FrameClock(): the network's diagnostics look
X86_EXTERN_FUNC(__imp__sub_001538C0);
X86_FUNC(sub_001538C0) {
    game::NetDiagFrame();
    if (game::NativeFrameClock(ctx, base)) return;
    __imp__sub_001538C0(ctx, base);
}

// GoToLevel(config): a System Link host's bots put in first
X86_EXTERN_FUNC(__imp__sub_000658D0);
X86_FUNC(sub_000658D0) {
    game::NetBotsConfig(kernel::R32(ctx.esp + 4));
    __imp__sub_000658D0(ctx, base);
}
