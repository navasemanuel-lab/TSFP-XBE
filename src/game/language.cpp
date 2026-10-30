// The game's language. The US game holds the PAL one's five (menus and text data/str_ts_<l>px, voices
// pak/sounds_<l>.pak), by the index at 0x4C0454: 0 English, 1 French, 2 German, 3 Italian, 4 Spanish. It takes the
// console's language (0x70360) only outside North America and Japan (0x74CF60, from the game region), so here it would
// stay English: the language setting (app::Settings::language) is put in before the text is loaded (0x3DD20) and after
// the game picks its own.
#include "app/app.h"
#include "core/log.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kLanguage = 0x4C0454;

// the setting's XC_LANGUAGE as the game's index, -1 to leave the game's
int32_t Index() {
    switch (app::Get().language) {
        case 3: return 2;  // German
        case 4: return 1;  // French
        case 5: return 4;  // Spanish
        case 6: return 3;  // Italian
        default: return -1;
    }
}

void Apply() {
    int32_t i = Index();
    if (i < 0 || kernel::R32(kLanguage) == uint32_t(i)) return;
    kernel::W32(kLanguage, uint32_t(i));
    LOG_FIRST(4, "language: the game's language %d", i);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00070360);
X86_FUNC(sub_00070360) {
    __imp__sub_00070360(ctx, base);
    Apply();
}

X86_EXTERN_FUNC(__imp__sub_0003DD20);
X86_FUNC(sub_0003DD20) {
    Apply();
    __imp__sub_0003DD20(ctx, base);
}
