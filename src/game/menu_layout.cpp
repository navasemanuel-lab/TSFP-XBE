// The main menu's rows in smaller letters, after the TimeSplitters Future Perfect PC port
// (TheDoffman/timesplitters-future-perfect-pc): with Quit to Desktop added its box grew up over the logo and the line
// under it. Its builder measures the rows and its row drawing draws them at the glyph scale (0x7E7270 across,
// 0x7E7280 down, made current by sub_0003F8D0); both run with it at three quarters, the sprites queued before flushed
// first so they keep theirs, and the scale put back after. Video Options is built and drawn the same way, its letters
// smaller still: its box is as narrow as the game's own pause boxes, between the pause screen's health and shield
// gauges, and its fourteen rows stay under the logo. The pause menu too, at the main menu's scale and narrower than the
// console's 62% (56%), so its corners stay clear of the gauges.
#include <functional>

#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game {
uint32_t VideoBuildCallback();  // game/video_menu.cpp, 0 before the menu is first opened
void StoryMenuForFour(bool building);  // game/story_four.cpp
}

namespace {

constexpr uint32_t kScene = 0x79094C;  // over 100 in the front end
constexpr uint32_t kGlyphX = 0x7E7270, kGlyphY = 0x7E7280;
constexpr uint32_t kFlushSprites = 0x22FF0, kApplyGlyphs = 0x3F8D0;
constexpr uint32_t kMainMenuBuild = 0x2D0BE0;  // the main menu descriptor's build callback
constexpr uint32_t kPauseBuild = 0x2F5520;     // the pause menu descriptor's (0x524700)
constexpr uint32_t kPauseTitle = 0x2F55AD;     // its title set, sub_00075B70(flags, title, width percent, 0, back)
constexpr float kPauseWidth = 62.0f, kPauseNarrow = 56.0f;
constexpr float kScale = 0.75f, kVideoScale = 0.56f;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

template <class F>
void Smaller(X86Context& ctx, F original, float scale = kScale) {
    uint32_t x = kernel::R32(kGlyphX), y = kernel::R32(kGlyphY);
    kernel::CallGuest(ctx, kFlushSprites, {});
    kernel::G<float>(kGlyphX) *= scale;
    kernel::G<float>(kGlyphY) *= scale;
    kernel::CallGuest(ctx, kApplyGlyphs, {});
    original();
    kernel::CallGuest(ctx, kFlushSprites, {});
    kernel::W32(kGlyphX, x);
    kernel::W32(kGlyphY, y);
    kernel::CallGuest(ctx, kApplyGlyphs, {});
}

}  // namespace

namespace game {
void WithVideoText(X86Context& ctx, const std::function<void()>& f) { Smaller(ctx, f, kVideoScale); }
void WithTextScale(X86Context& ctx, const std::function<void()>& f, float scale) { Smaller(ctx, f, scale); }
}  // namespace game

// a menu drawn (menu, pass): Video Options and the pause menu all at their smaller scales
X86_EXTERN_FUNC(__imp__sub_00076810);
X86_FUNC(sub_00076810) {
    uint32_t menu = kernel::R32(ctx.esp + 4), video = game::VideoBuildCallback();
    uint32_t build = Valid(menu, 8) && Valid(kernel::R32(menu + 4), 4) ? kernel::R32(kernel::R32(menu + 4)) : 0;
    if (build && build == video) {
        Smaller(ctx, [&] { __imp__sub_00076810(ctx, base); }, kVideoScale);
    } else if (build == kPauseBuild) {
        Smaller(ctx, [&] { __imp__sub_00076810(ctx, base); });
    } else {
        __imp__sub_00076810(ctx, base);
    }
}

// the pause menu's builder
X86_EXTERN_FUNC(__imp__sub_002F5520);
X86_FUNC(sub_002F5520) { Smaller(ctx, [&] { __imp__sub_002F5520(ctx, base); }); }

// a menu's title and width: the pause menu's narrower (its split-screen 80% kept)
X86_EXTERN_FUNC(__imp__sub_00075B70);
X86_FUNC(sub_00075B70) {
    if (kernel::R32(ctx.esp) == kPauseTitle && kernel::G<float>(ctx.esp + 12) == kPauseWidth)
        kernel::G<float>(ctx.esp + 12) = kPauseNarrow;
    __imp__sub_00075B70(ctx, base);
}

// the main menu's builder, its Story row open to three and four players (game/story_four.cpp)
X86_EXTERN_FUNC(__imp__sub_002D0BE0);
X86_FUNC(sub_002D0BE0) {
    game::StoryMenuForFour(true);
    if (int32_t(kernel::R32(kScene)) <= 100) __imp__sub_002D0BE0(ctx, base);
    else Smaller(ctx, [&] { __imp__sub_002D0BE0(ctx, base); });
    game::StoryMenuForFour(false);
}

// a menu row drawn (menu, item): the main menu's text rows (type 7)
X86_EXTERN_FUNC(__imp__sub_00076150);
X86_FUNC(sub_00076150) {
    uint32_t menu = kernel::R32(ctx.esp + 4), item = kernel::R32(ctx.esp + 8);
    bool main_row = int32_t(kernel::R32(kScene)) > 100 && Valid(menu, 0x23C) && Valid(item, 0x8C) &&
                    kernel::R32(item) == 7 && kernel::R32(item + 0x88) == menu && Valid(kernel::R32(menu + 4), 4) &&
                    kernel::R32(kernel::R32(menu + 4)) == kMainMenuBuild;
    if (!main_row) {
        __imp__sub_00076150(ctx, base);
        return;
    }
    Smaller(ctx, [&] { __imp__sub_00076150(ctx, base); });
}
