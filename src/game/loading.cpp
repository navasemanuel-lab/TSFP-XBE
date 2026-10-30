// Fast loading. The loading screen's thread (0x156CB0) moves the bar it shows (float 0x749848, 0 to 1) a step each
// frame toward the stage the load has reached, at a pace set for the console's disc. At the end of a load 0x156840
// sets the last stage and waits until the bar is full: on a PC the load takes a fraction of a second and most of the
// screen was that wait. With fast_loading the bar is full before the wait.
#include "common/options.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kBarShown = 0x749848;
constexpr uint32_t kBarDone = 0x4E7A94;  // set once the loading screen has ended
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00156840);
X86_FUNC(sub_00156840) {
    if (game::Opt().fast_loading && !kernel::R32(kBarDone)) kernel::G<float>(kBarShown) = 1.0f;
    __imp__sub_00156840(ctx, base);
}
