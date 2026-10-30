// Rain off (Options::rain, the settings panel's General page): the lightning's streaks (0x119B20, about a second in
// every seven while [0x73D9A4] runs 0 to 1, drawn as lines with the rain2 sprite by 0x118D90) and the drops that run
// down the screen (0x118240, the camsplash sprite, from the view's frame 0x119F30) are not drawn. The falling rain is
// weather particles from 0x11BBC0 and stays. Their sounds stay.
#include "common/options.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// DrawLightning(): the lightning's streaks
X86_EXTERN_FUNC(__imp__sub_00119B20);
X86_FUNC(sub_00119B20) {
    if (game::Opt().rain) __imp__sub_00119B20(ctx, base);
    else kernel::Return(ctx, 0, 0);
}

// DrawScreenDrops(count, size): the drops on the screen, the caller pops its arguments
X86_EXTERN_FUNC(__imp__sub_00118240);
X86_FUNC(sub_00118240) {
    if (game::Opt().rain) __imp__sub_00118240(ctx, base);
    else kernel::Return(ctx, 0, 0);
}
