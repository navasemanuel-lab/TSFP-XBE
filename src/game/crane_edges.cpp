// The crane's view (Scotland) and the remote views drawn with it: 0x137010 frames the screen with four mirrored
// holoedge quadrants and fills the sides outside them with 0x1EFB0 (x0, y0, x1, y1, colour), the right and bottom
// edges taken from the screen's box at [0x7E71C8] (+4, +0x18), which are its last pixel (639, 479). The fill ends
// there, so the console's last column keeps the bright scene beside the dark border: a line nobody sees on a TV,
// a strip of host pixels upscaled. Inside 0x137010 a fill to the last column or row reaches the screen's edge.
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {
thread_local bool g_in_frame = false;
}

X86_EXTERN_FUNC(__imp__sub_00137010);
X86_FUNC(sub_00137010) {
    g_in_frame = true;
    __imp__sub_00137010(ctx, base);
    g_in_frame = false;
}

// FillRect(x0, y0, x1, y1, colour), the caller pops its arguments
X86_EXTERN_FUNC(__imp__sub_0001EFB0);
X86_FUNC(sub_0001EFB0) {
    if (g_in_frame) {
        uint32_t box = kernel::R32(0x7E71C8);
        uint32_t x1 = ctx.esp + 12, y1 = ctx.esp + 16;  // after the return address: x0, y0, x1, y1
        if (box && kernel::R32(x1) == kernel::R32(box + 4)) kernel::W32(x1, kernel::R32(box + 4) + 1);
        if (box && kernel::R32(y1) == kernel::R32(box + 0x18)) kernel::W32(y1, kernel::R32(box + 0x18) + 1);
    }
    __imp__sub_0001EFB0(ctx, base);
}
