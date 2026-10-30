// Blood off (Options::blood, the settings panel's Graphics page): what a hit bleeds is not made. The blood is the game's
// module at 0x345700: the sprays (0x345B60, emitters 0x766AD0/0x766CB0/0x766C28 by kind, and 0x345BB0, 0x766E08, set up
// with the blood sprites z_blood* at 0x345810), the splats on walls and floors around a hit (0x345C60, a decal with one
// of the z_blood* sprites, 0x345710) and the pool under a body (0x345CC0, the bloodpool sprite). They draw on the
// game's random numbers, so online the setting is the host's on both machines.
#include "common/options.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// SpawnBlood(kind, ...): a spray, the caller pops its arguments
X86_EXTERN_FUNC(__imp__sub_00345B60);
X86_FUNC(sub_00345B60) {
    if (game::Opt().blood) __imp__sub_00345B60(ctx, base);
    else kernel::Return(ctx, 0, 0);
}

X86_EXTERN_FUNC(__imp__sub_00345BB0);
X86_FUNC(sub_00345BB0) {
    if (game::Opt().blood) __imp__sub_00345BB0(ctx, base);
    else kernel::Return(ctx, 0, 0);
}

// BloodSplat(...): the decal, 0 when none was placed (the callers then try another way)
X86_EXTERN_FUNC(__imp__sub_00345C60);
X86_FUNC(sub_00345C60) {
    if (game::Opt().blood) __imp__sub_00345C60(ctx, base);
    else kernel::Return(ctx, 0, 0);
}

// BloodPool(character)
X86_EXTERN_FUNC(__imp__sub_00345CC0);
X86_FUNC(sub_00345CC0) {
    if (game::Opt().blood) __imp__sub_00345CC0(ctx, base);
    else kernel::Return(ctx, 0, 0);
}
