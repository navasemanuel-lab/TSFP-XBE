// MapMaker's cursor follows the mouse. Each player's cursor (0x7BA260 + player * 0x1E0) keeps its place as a percent
// at +0x18 and +0x1C and in UI canvas pixels at +0x20 and +0x24, within +0x48..+0x54; pixels are the percent times
// the scale at 0x7BA038 (x) and 0x7BA188 (y), plus the origin at 0x7BA180 and 0x7BA184. The stick still moves it:
// the mouse sets it only when it has moved.
#include <algorithm>
#include <bit>
#include <cmath>

#include "common/mouse.h"
#include "xdk/hle.h"

namespace game {
void MapMakerEditorSeen();  // game/discord.cpp: the presence shows the editor
}

namespace {

constexpr uint32_t kMenuPlayer = 0x7BA9E0;
constexpr uint32_t kCursors = 0x7BA260, kCursorSize = 0x1E0;
constexpr uint32_t kScaleX = 0x7BA038, kScaleY = 0x7BA188, kOriginX = 0x7BA180, kOriginY = 0x7BA184;
constexpr uint32_t kCanvasW = 0x4E8CF0, kCanvasH = 0x4E8CF4;

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void SetF(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }

void FollowMouse() {
    if (kernel::R32(kMenuPlayer) != 0) return;  // the keyboard and mouse are player 1's
    mouse::InGameCursor();
    static float last_x = -1, last_y = -1;
    mouse::Pointer p = mouse::MenuPointer(false);
    if (!p.valid || (p.x == last_x && p.y == last_y)) return;
    last_x = p.x;
    last_y = p.y;
    uint32_t c = kCursors;
    float sx = F(kScaleX), sy = F(kScaleY), ox = F(kOriginX), oy = F(kOriginY);
    if (!(std::fabs(sx) > 1e-3f && std::fabs(sy) > 1e-3f && std::isfinite(ox) && std::isfinite(oy))) return;
    float x = p.x * float(kernel::R32(kCanvasW)), y = p.y * float(kernel::R32(kCanvasH));
    float x0 = F(c + 0x48), y0 = F(c + 0x4C), x1 = F(c + 0x50), y1 = F(c + 0x54);
    if (x0 < x1) x = std::clamp(x, x0, x1);
    if (y0 < y1) y = std::clamp(y, y0, y1);
    SetF(c + 0x20, x);
    SetF(c + 0x24, y);
    SetF(c + 0x18, (x - ox) / sx);
    SetF(c + 0x1C, (y - oy) / sy);
}

}  // namespace

// DrawCursor(): the current menu player's cursor, each frame it shows
X86_EXTERN_FUNC(__imp__sub_0030CF60);
X86_FUNC(sub_0030CF60) {
    FollowMouse();
    game::MapMakerEditorSeen();
    __imp__sub_0030CF60(ctx, base);
}
