// The health and shield gauges on Story's Objectives screen, after the TimeSplitters Future Perfect PC port
// (TheDoffman/timesplitters-future-perfect-pc). The Back/Tab status screen sets 0x100 in the player's pause state, which
// the menu renderer takes to leave the gauges out; it is masked for the drawing alone (it also steers the menu's input
// and the pause), so Story draws its own gauges with their animations and split-screen layout. The gauges and the
// objectives panel beside them are fitted inside the player's view, shrunk only, when they would overflow it. On the
// pause screens they stay put at one size: the game eases their scale between each menu's own as the pages change
// (0x2E5750, [menu+0x210]), here they keep the one it gives with no menu.
#include <algorithm>
#include <bit>
#include <cstring>

#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game {
bool DrawEarlyArcs(X86Context& ctx);  // game/hud_style.cpp
}

namespace {

constexpr uint32_t kPlayer = 0x7B0C7C;
constexpr uint32_t kGameMode = 0x7DE454;   // byte 1: 10 in Story
constexpr uint32_t kPauseStates = 0x761C48;  // one per local slot
constexpr uint32_t kPlayers = 0x790950;
constexpr uint32_t kView = 0x7E71C8;       // the current HUD viewport: +0 left, +4 right, +8 x origin, +0x14 top, +0x18
                                           // bottom, +0x1C y origin
constexpr uint32_t kGaugesFromObjectives = 0x2E58C9;  // the pause menus' renderer, Objectives among them
constexpr uint32_t kGaugeScale = 0x4C0460, kNoMenuScale = 0x49E5EC;  // the pause gauges' scale: their product at rest
constexpr uint32_t kPanelWidthA = 0x47D1F4, kPanelWidthB = 0x478628;  // the objectives builder's percentage widths

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void SetF(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }
bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }

// the gauges' scale, shrunk so they fit the viewport with a 4% margin; one factor for both axes
float FitScale(float scale, float left, float top, float right, float bottom, float cx, float cy, uint32_t players,
               uint32_t index) {
    if (!(scale > 0) || !(right > left) || !(bottom > top) || players < 1 || players > 4 || index >= players) return scale;
    bool narrow = players > 2 && !(players == 3 && index == 0);
    float half_w = narrow ? 300.0f * 1.08f * 0.5f : 512.0f * 1.2f * 0.5f;
    float half_h = (players == 1 ? 340.0f : 220.0f) * 0.5f;
    float mx = (right - left) * 0.04f, my = (bottom - top) * 0.04f;
    float limit = std::min({(cx - left - mx) / half_w, (right - cx - mx) / half_w, (cy - top - my) / half_h,
                            (bottom - cy - my) / half_h});
    limit = std::max(limit, 0.0f);
    return std::min(scale, limit);
}

struct Box {
    float left, top, right, bottom;
};
Box ViewBox(uint32_t view) {
    return {float(int32_t(kernel::R32(view))), float(int32_t(kernel::R32(view + 0x14))),
            float(int32_t(kernel::R32(view + 4))) + 1.0f, float(int32_t(kernel::R32(view + 0x18))) + 1.0f};
}

}  // namespace

// the menu renderer

X86_EXTERN_FUNC(__imp__sub_002E5750);
X86_FUNC(sub_002E5750) {
    uint32_t player = kernel::R32(kPlayer), state = 0, saved = 0;
    if (player && Story()) {
        uint32_t slot = kernel::R32(player + 4);
        if (slot < 4) {
            uint32_t s = kernel::R32(kPauseStates + slot * 4);
            if (Valid(s, 4) && (kernel::R32(s) & 0x101) == 0x101) {
                state = s;
                saved = kernel::R32(s) & 0x180;  // 0x80 too: only the full pause path clears it
                kernel::W32(s, kernel::R32(s) & ~0x100u);
            }
        }
    }
    __imp__sub_002E5750(ctx, base);
    if (state) kernel::W32(state, (kernel::R32(state) & ~0x180u) | saved);
}

// the objectives panel's builder: its widths shrunk as the gauges beside it
X86_EXTERN_FUNC(__imp__sub_002F5E60);
X86_FUNC(sub_002F5E60) {
    uint32_t player = kernel::R32(kPlayer);
    uint32_t cam = player ? kernel::R32(player + 8) : 0;
    if (!cam || !Story() || !Valid(cam + 0x224, 0x20)) {
        __imp__sub_002F5E60(ctx, base);
        return;
    }
    Box b = ViewBox(cam + 0x224);  // the camera's HUD viewport
    float ratio = FitScale(1.2f, b.left, b.top, b.right, b.bottom, (b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f,
                           kernel::R32(kPlayers), kernel::R32(player + 4)) / 1.2f;
    uint32_t a = kernel::R32(kPanelWidthA), w = kernel::R32(kPanelWidthB);
    SetF(kPanelWidthA, F(kPanelWidthA) * ratio);
    SetF(kPanelWidthB, F(kPanelWidthB) * ratio);
    __imp__sub_002F5E60(ctx, base);
    kernel::W32(kPanelWidthA, a);
    kernel::W32(kPanelWidthB, w);
}

// the health and shield gauges (..., x, y, scale): from the Objectives screen, fitted to the view
X86_EXTERN_FUNC(__imp__sub_0013AB60);
X86_FUNC(sub_0013AB60) {
    uint32_t sp = ctx.esp, view = kernel::R32(kView), player = kernel::R32(kPlayer);
    if (kernel::R32(sp) != kGaugesFromObjectives || !Valid(view, 0x20)) {
        if (game::DrawEarlyArcs(ctx)) return;  // the view's own, as TimeSplitters 2's in the early style
        __imp__sub_0013AB60(ctx, base);
        return;
    }
    uint32_t saved = kernel::R32(sp + 16);
    SetF(sp + 16, F(kGaugeScale) * F(kNoMenuScale));
    if (player && Story()) {
        Box b = ViewBox(view);
        float x = F(sp + 8) + float(int32_t(kernel::R32(view + 8))) - b.left;
        float y = F(sp + 12) + float(int32_t(kernel::R32(view + 0x1C))) - b.top;
        SetF(sp + 16, FitScale(F(sp + 16), b.left, b.top, b.right, b.bottom, x, y, kernel::R32(kPlayers),
                               kernel::R32(player + 4)));
    }
    // TimeSplitters 2's in the early style, at this scale
    if (!game::DrawEarlyArcs(ctx)) __imp__sub_0013AB60(ctx, base);
    kernel::W32(sp + 16, saved);
}
