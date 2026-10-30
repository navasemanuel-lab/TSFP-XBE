// Skip any cutscene: Start or A skips the cutscenes the game marks unskippable too.
// The cutscene flags at 0x7DE458 say what the skip check is for: 0x20000000 skippable, the check's yes asks for the
// skip; 0x40000000 unskippable, the game checks the pads and drops the answer (the first viewing of story scenes).
#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kCutsceneFlags = 0x7DE458;
constexpr uint32_t kUnskippable = 0x40000000;
constexpr uint32_t kRequestSkip = 0x98DB0;  // (0): what the game calls for a skippable cutscene
}  // namespace

// SkipPressed(): Start or A newly pressed on a connected pad
X86_EXTERN_FUNC(__imp__sub_00065FE0);
X86_FUNC(sub_00065FE0) {
    __imp__sub_00065FE0(ctx, base);
    uint32_t pressed = ctx.eax;
    static int calls = 0;
    if (uint32_t scene = kernel::R32(0x70E164); scene && (pressed || ++calls % 120 == 0))
        LOGI("cutscene: pressed %u flags %08X/%08X scene %08X req %u fade %08X +20 %u timer %08X 4D977C %u", pressed,
             kernel::R32(kCutsceneFlags), kernel::R32(0x7DE398), scene, kernel::R32(scene + 0x1C), kernel::R32(scene + 0x18),
             kernel::R32(scene + 0x20), kernel::R32(0x70AAF0), kernel::R32(0x4D977C));
    if (pressed && game::Opt().skip_any_cutscene && (kernel::R32(kCutsceneFlags) & kUnskippable)) {
        kernel::CallGuest(ctx, kRequestSkip, {0});
        ctx.eax = pressed;
    }
}
