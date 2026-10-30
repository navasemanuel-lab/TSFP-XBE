// Every character's whole voice in Story. A character type (0x53A3C8, 0x30 each) points at its voice (+4): up to 18
// pain sounds (+0), up to 6 death sounds (+0x24), and how many of each the level loads and picks from (+0x38, +0x3C),
// counted by 0xD76D0 (ESI the voice, the type on the stack). Arcade takes them all; Story (mode [0x7DE455] 10) takes
// half of each for the enemies and one death sound for the players' characters, to keep the console's memory, so a
// level's soldiers die with the same one or two cries. Here Story counts them all too.
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kPain = 0, kPainMost = 18, kDeath = 0x24, kDeathMost = 6, kPainCount = 0x38, kDeathCount = 0x3C;

uint32_t Count(uint32_t at, uint32_t most) {
    uint32_t n = 0;
    while (n < most && kernel::R16(at + n * 2)) n++;
    return n;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000D76D0);
X86_FUNC(sub_000D76D0) {
    uint32_t voice = ctx.esi, type = kernel::R32(ctx.esp + 4);
    __imp__sub_000D76D0(ctx, base);
    uint32_t pain = Count(voice + kPain, kPainMost), death = Count(voice + kDeath, kDeathMost);
    uint32_t had_pain = kernel::R32(voice + kPainCount), had_death = kernel::R32(voice + kDeathCount);
    if (had_pain == pain && had_death == death) return;
    kernel::W32(voice + kPainCount, pain);
    kernel::W32(voice + kDeathCount, death);
    static int logged = 0;
    if (logged++ < 64)
        LOGR("voices: type %u with %u pain and %u death sounds (the game's %u and %u)", type, pain, death, had_pain,
             had_death);
}
