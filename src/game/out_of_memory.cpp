// The game's out of memory report. Alloc(size, kind) (0x3DFA0) hands out the level's memory: kinds 0 (from the top,
// after a 0x10 "LOCLMEM" header) and 1 (from the bottom) share the free count at 0x6B8384. Asked for more than is free,
// the console's game calls its report (0x389B0, an empty function in the retail build) and goes on: the block given
// overlaps what lies beyond and the game crashes later somewhere else. The log says when it happens, and who asked.
//
// The level's memory is one block taken at start (0x23CE9, XPhysicalAlloc of the size at 0x4C0450) and set up for each
// level from that size (0x3C34E). Mods' models are larger than the console's (the TS2 weapons fill Machine Wars past
// it), so the block is taken a little larger when the memory is there.
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kFree = 0x6B8384;
constexpr uint32_t kLevelSize = 0x4C0450, kLevelAlloc = 0x23CEE, kConsoleSize = 0x2CEC000;
constexpr uint32_t kExtra = 0x100000;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0037D3E9);
X86_FUNC(sub_0037D3E9) {
    uint32_t size = kernel::R32(ctx.esp + 4);
    if (kernel::R32(ctx.esp) == kLevelAlloc && size == kConsoleSize) {
        if (uint32_t probe = mem::AllocContiguous(size + kExtra, 0, 0xFFFFFFFF, 0x1000)) {
            mem::FreeContiguous(probe);
            size += kExtra;
            kernel::W32(ctx.esp + 4, size);
            kernel::W32(kLevelSize, size);
        }
        LOGI("memory: the level's memory is %u KB (the console's %u KB)", size >> 10, kConsoleSize >> 10);
    }
    __imp__sub_0037D3E9(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0003DFA0);
X86_FUNC(sub_0003DFA0) {
    uint32_t size = (kernel::R32(ctx.esp + 4) + 15) & ~15u, kind = kernel::R32(ctx.esp + 8);
    if (kind <= 1) {
        static int reported = 0;
        int32_t free = int32_t(kernel::R32(kFree)), need = int32_t(size + (kind == 0 ? 0x10 : 0));
        if (need > free && reported < 20) {
            reported++;
            LOGR("memory: out of the level's memory: %u bytes asked (kind %u, from %08X), %d free", size, kind,
                 kernel::R32(ctx.esp), free);
        }
    }
    __imp__sub_0003DFA0(ctx, base);
}
