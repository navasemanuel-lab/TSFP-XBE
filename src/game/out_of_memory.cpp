// The game's out of memory report. Alloc(size, kind) (0x3DFA0) hands out the level's memory: kinds 0 (from the top,
// after a 0x10 "LOCLMEM" header) and 1 (from the bottom) share the free count at 0x6B8384. Asked for more than is free,
// the console's game calls its report (0x389B0, an empty function in the retail build) and goes on: the block given
// overlaps what lies beyond and the game crashes later somewhere else. The log says when it happens, and who asked.
//
// The level's memory is one block taken at start (0x23CE9, XPhysicalAlloc of the size at 0x4C0450) and set up for each
// level from that size (0x3C34E). Mods' models and textures are larger than the console's (the TS2 weapons filled
// Machine Wars past it; the beta Cortez, a Story character of one's choice and the TS2 weapons together Scotland by
// 217 KB), so the block is taken larger: the most of kExtras that leaves kSpare of the console's memory free beside it.
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kFree = 0x6B8384;
constexpr uint32_t kLevelSize = 0x4C0450, kLevelAlloc = 0x23CEE, kConsoleSize = 0x2CEC000;
constexpr uint32_t kExtras[] = {8u << 20, 6u << 20, 4u << 20, 2u << 20, 1u << 20};
constexpr uint32_t kSpare = 8u << 20;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0037D3E9);
X86_FUNC(sub_0037D3E9) {
    uint32_t size = kernel::R32(ctx.esp + 4);
    if (kernel::R32(ctx.esp) == kLevelAlloc && size == kConsoleSize) {
        for (uint32_t extra : kExtras) {
            uint32_t probe = mem::AllocContiguous(size + extra + kSpare, 0, 0xFFFFFFFF, 0x1000);
            if (!probe) continue;
            mem::FreeContiguous(probe);
            size += extra;
            kernel::W32(ctx.esp + 4, size);
            kernel::W32(kLevelSize, size);
            break;
        }
        LOGR("memory: the level's memory is %u KB (the console's %u KB)", size >> 10, kConsoleSize >> 10);
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
