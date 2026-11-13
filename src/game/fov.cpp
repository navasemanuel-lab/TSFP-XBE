// The field of view and the screen's shape, set from the Video Options menu (game/video_menu.cpp).
//
// The view, after the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc): the camera
// ([0x7B0CC0]) is built by sub_000C77C0, which writes its projection (matrix at +0x204) and the tangents of its half
// angles, vertical +0x37C and horizontal +0x380, that culling uses. On foot, both are widened by tan(fov/2) /
// tan(55/2) after it: the zoomed views keep their ratio to the view, the cutscene, terminal and front end cameras
// their own lens.
//
// The polygon offsets: marks laid over a surface (grime, shadows, the decals below) win over it by a depth offset partly
// scaled by each pixel's depth step, which a widened view makes larger by as much: drawn as it is, the grime over a wall
// came out over a sign just in front of that wall too, and blacked it out as the angle grew. The renderer takes the
// slope back by the widening (gpu::g_view_widening).
//
// The decals (bullet marks and other marks on walls), after the PC port too: their two passes (sub_00112EA0 and
// sub_00115A40) build a lens of their own with sub_00151CF0, with the depth pulled in so the marks win over the wall
// under them, from the game's angle: with the view widened they were drawn through a narrower lens than the wall and
// floated off it. Their lens takes the camera's two scales (+0 and +0x14), its depth terms kept.
//
// The weapon, after lmplied/TSFPRecomp: it is drawn with a lens of its own, built by sub_001B73F0 from the camera's
// angle (+0x384, not widened above) times the constant at 0x47E298, 1/55, times 45: the weapon keeps its 45 degrees
// whatever the view's. The constant becomes (weapon / 45) / 55.
//
// The hands: the arms holding the weapon are a model of their own ([character+0xE84]), shown by sub_001BF410 for each
// held weapon (bit 0 of +0x28, as the weapon's own) and placed where the weapon is. With the hands off that bit goes
// back off after it, and the weapon is drawn alone.
//
// In a network session (System Link or online) everyone has the game's own angles: no wider view than the others.
//
// Each player has angles of their own (Opt().fov and weapon_fov player 1's): the view built is the player's being
// updated, the weapon lens the player's whose camera is the current one.
//
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace game {

namespace {

constexpr uint32_t kCamera = 0x7B0CC0, kPlayer = 0x7B0C7C;
constexpr uint32_t kScene = 0x79094C;       // over 100 in the front end, whose scenes use a player camera
constexpr uint32_t kStoryFlags = 0x7DE458;  // 0x20000000: the story renderer's own view
constexpr uint32_t kLetterboxDone = 0x7490E8, kLetterboxPhase = 0x7490EC;
constexpr uint32_t kWeaponLens = 0x47E298;
constexpr uint32_t kShapeW = 0x4C0458, kShapeH = 0x4C045C, kSqueeze = 0x4C0460;
// the two player camera builds in sub_000CCAB0
constexpr uint32_t kFromPlay1 = 0xCCBED, kFromPlay2 = 0xCDD35;
// the decal passes' lens builds
constexpr uint32_t kFromDecals1 = 0x112F7F, kFromDecals2 = 0x115B0C;
constexpr float kGameFov = 55.0f, kGameWeaponFov = 45.0f;
constexpr uint32_t kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584, kPlayers = 0x790950;
constexpr uint32_t kSession = 0x76B0FC, kSessionUp = 0x76B108;  // a network session, up (game/discord.cpp)

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

// a player's slot, 0 to 3, by their address; 0 for none
uint32_t SlotOf(uint32_t player) {
    uint32_t first = kernel::R32(kPlayerArray);
    if (player < first || !first) return 0;
    uint32_t slot = (player - first) / kPlayerSize;
    return slot < 4 && slot < kernel::R32(kPlayers) && player == first + slot * kPlayerSize ? slot : 0;
}

// the slot of the player whose camera this is; 0 for none
uint32_t CameraSlot(uint32_t camera) {
    uint32_t first = kernel::R32(kPlayerArray), n = std::min<uint32_t>(kernel::R32(kPlayers), 4);
    if (!camera || !Valid(first, n * kPlayerSize)) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (kernel::R32(first + i * kPlayerSize + 8) == camera) return i;
    return 0;
}

// the view's widening for an angle
float ScaleOf(uint32_t fov) {
    if (fov == uint32_t(kGameFov) || fov < 20 || fov > 140) return 1.0f;
    constexpr float kHalfDegree = 3.14159265f / 360.0f;
    return std::tan(float(fov) * kHalfDegree) / std::tan(kGameFov * kHalfDegree);
}
float Scale(uint32_t slot) { return FovLocked() ? 1.0f : ScaleOf(Fov(slot)); }

// widens the camera's lens and culling by the scale
void Widen(uint32_t camera, float scale) {
    uint32_t matrix = kernel::R32(camera + 0x204);
    if (!Valid(matrix, 0x18)) return;
    kernel::G<float>(matrix) /= scale;
    kernel::G<float>(matrix + 0x14) /= scale;
    kernel::G<float>(camera + 0x37C) *= scale;
    kernel::G<float>(camera + 0x380) *= scale;
}

// the player's own view on foot or in the air (states 2 and 4), not a letterbox transition
bool PlayerView(uint32_t caller) {
    if (caller != kFromPlay1 && caller != kFromPlay2) return false;
    if (int32_t(kernel::R32(kScene)) > 100 || (kernel::R32(kStoryFlags) & 0x20000000)) return false;
    if (!kernel::R32(kLetterboxDone) && kernel::G<float>(kLetterboxPhase) > 0.0f) return false;
    uint32_t actor = kernel::R32(kPlayer);
    if (!Valid(actor, 0x118)) return false;
    uint32_t state = kernel::R32(actor + 0x114);
    return (state == 2 || state == 4) && kernel::R32(actor + 8) == kernel::R32(kCamera);
}

}  // namespace

bool FovLocked() { return kernel::R32(kSession) && kernel::R32(kSessionUp); }

void ApplyWeaponFov(uint32_t slot) {
    bool game = FovLocked();
    float weapon = game ? kGameWeaponFov : float(std::clamp<uint32_t>(WeaponFov(slot), 20, 120));
    kernel::G<float>(kWeaponLens) = weapon / kGameWeaponFov / kGameFov;
}
void ApplyWeaponFov() { ApplyWeaponFov(0); }

void SampleDiscord();  // game/discord.cpp

void ApplyScreenShape() {
    uint32_t w, h;
    app::AspectFraction(w, h);
    if (kernel::R32(kShapeW) == w && kernel::R32(kShapeH) == h) return;
    kernel::W32(kShapeW, w);
    kernel::W32(kShapeH, h);
    kernel::G<float>(kSqueeze) = (4.0f * h) / (3.0f * w);
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_000C77C0);
X86_FUNC(sub_000C77C0) {
    uint32_t caller = kernel::R32(ctx.esp);
    // the shape as set, each frame: the game puts back a copy it took before a profile's own (0x434CE), which a
    // shape chosen since is not in
    game::ApplyScreenShape();
    __imp__sub_000C77C0(ctx, base);
    game::SampleDiscord();  // each frame, in play and the front end
    bool view = game::PlayerView(caller);
    uint32_t slot = game::SlotOf(kernel::R32(game::kPlayer)), camera = kernel::R32(game::kCamera);
    if (!view) {
        // a player's camera in a cutscene or the like: the game's own angle
        if (caller == game::kFromPlay1 || caller == game::kFromPlay2) gpu::g_view_widening = 1.0f;
        return;
    }
    float scale = game::Scale(slot);
    if (scale == 1.0f || !game::Valid(camera, 0x384)) {
        gpu::g_view_widening = 1.0f;
        return;
    }
    game::Widen(camera, scale);
    gpu::g_view_widening = scale;  // the polygon offsets' slopes taken back by as much (gpu/d3d12.cpp)
}

// the held weapon's lens (from the current camera's angle): the angle of the player whose camera it is
X86_EXTERN_FUNC(__imp__sub_001B73F0);
X86_FUNC(sub_001B73F0) {
    game::ApplyWeaponFov(game::CameraSlot(kernel::R32(game::kCamera)));
    __imp__sub_001B73F0(ctx, base);
}

// a held weapon placed for the frame (eax its slot in the current character): the arms shown with it, hidden again;
// with the free camera out the weapon too
X86_EXTERN_FUNC(__imp__sub_001BF410);
X86_FUNC(sub_001BF410) {
    uint32_t held = ctx.eax;
    __imp__sub_001BF410(ctx, base);
    bool first = kernel::R32(game::kPlayer) == kernel::R32(game::kPlayerArray);  // player 1's
    bool free = game::FreeCamOn() && first;
    // the free camera (free_cam.cpp) shows neither: the weapon's model is [held + 0x54]. The game doesn't show it again
    // each frame, so the one hidden is shown again once the camera is back.
    static uint32_t hidden = 0;
    uint32_t model = game::Valid(held, 0x58) ? kernel::R32(held + 0x54) : 0;
    if (!game::Valid(model, 0x2C)) model = 0;
    if (free && model) {
        if (kernel::R32(model + 0x28) & 1u) hidden = model;
        kernel::W32(model + 0x28, kernel::R32(model + 0x28) & ~1u);
    } else if (first && hidden) {
        if (game::Valid(hidden, 0x2C)) kernel::W32(hidden + 0x28, kernel::R32(hidden + 0x28) | 1u);
        hidden = 0;
    }
    if (game::Opt().hands && !free) return;
    uint32_t chr = kernel::R32(game::kPlayer);
    if (!game::Valid(chr, 0xE88)) return;
    uint32_t arms = kernel::R32(chr + 0xE84);
    if (game::Valid(arms, 0x2C)) kernel::W32(arms + 0x28, kernel::R32(arms + 0x28) & ~1u);
}

// the black bars over the view (two shaded rectangles by the letterbox phase): not drawn with the option off
namespace {

// a pause, objectives or status screen is up (the menus' own test, menu_mouse.cpp): it uses the bars, so they stay
bool MenuUp() {
    uint32_t pause = kernel::R32(0x761C48);
    return pause >= 0x10000 && mem::IsCommitted(pause) && (kernel::R32(pause) & 1);
}

// the bars are left out of the view: the option off and no menu over it
bool BarsOff() { return !game::Opt().letterbox && !MenuUp(); }

float Float(uint32_t at) {
    uint32_t v = kernel::R32(at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// the black bars of a cutscene (24 vertices) or the x-ray (12): one strip in screen units, a band across the top (0 to
// its edge), one across the bottom (its edge to 480) and, in a cutscene, a line down each side between them
bool LeaveOutBars(uint32_t prim, uint32_t start, uint32_t count, uint32_t positions, uint32_t stride) {
    if (!BarsOff() || prim != 5 || (count != 24 && count != 12)) return false;
    auto at = [&](uint32_t i, uint32_t axis) { return Float(positions + (start + i) * stride + axis * 4); };
    float top = at(2, 1), bottom = at(6, 1);
    return at(0, 0) == 0 && at(0, 1) == 0 && at(1, 0) == 640 && at(11, 0) == 640 && at(11, 1) == 480 &&
           at(8, 1) == 480 && top > 0 && top < bottom && bottom < 480 && at(5, 1) == top && at(7, 1) == bottom;
}

void PutFloat(uint32_t at, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    kernel::W32(at, v);
}

// the Mag-charger's x-ray works on a band of the screen, rows 64 to 416: a copy of the band to a 160x120 target (rows
// 16 to 104 of it), back from there, a darkening and a grain over it. With the bars gone the band is the whole screen:
// each of those quads' top and bottom go to the edge and its texture rows with them, as far as they run
bool WidenXray(uint32_t prim, uint32_t start, uint32_t count, uint32_t positions, uint32_t stride) {
    if (!BarsOff() || (count != 4 && count != 6)) return false;
    auto pos = [&](uint32_t i) { return positions + (start + i) * stride; };
    float lo = 1e9f, hi = -1e9f, x0 = 1e9f, x1 = -1e9f;
    for (uint32_t i = 0; i < count; i++) {
        float y = Float(pos(i) + 4), x = Float(pos(i));
        lo = std::min(lo, y), hi = std::max(hi, y);
        x0 = std::min(x0, x), x1 = std::max(x1, x);
    }
    float full;
    if (lo == 64 && hi == 416 && x0 == 0 && x1 == 640) full = 480;
    else if (lo == 16 && hi == 104 && x0 == 0 && x1 == 160) full = 120;
    else return false;
    for (uint32_t i = 0; i < count; i++) {
        float y = Float(pos(i) + 4);
        if (y != lo && y != hi) return false;
    }
    // the texture's rows, a pair of floats after the position and its next four bytes, where there are any
    float slope = 0;
    if (count == 4) {
        float vlo = 0, vhi = 0;
        for (uint32_t i = 0; i < count; i++) {
            (Float(pos(i) + 4) == lo ? vlo : vhi) = Float(pos(i) + 20);
        }
        slope = (vhi - vlo) / (hi - lo);
    }
    for (uint32_t i = 0; i < count; i++) {
        float y = Float(pos(i) + 4), edge = y == lo ? 0.0f : full;
        if (count == 4) PutFloat(pos(i) + 20, Float(pos(i) + 20) + (edge - y) * slope);
        PutFloat(pos(i) + 4, edge);
    }
    return false;
}

bool VerticesDrawn(uint32_t prim, uint32_t start, uint32_t count, uint32_t positions, uint32_t stride) {
    if (LeaveOutBars(prim, start, count, positions, stride)) return true;
    return WidenXray(prim, start, count, positions, stride);
}

const bool g_bars = [] {
    d3d::g_vertices_draw_hook = VerticesDrawn;
    return true;
}();


}  // namespace

// the game's clip rectangle (x0, y0, x1, y1): the x-ray and the cutscenes clip the view between the bars, rows 64 to
// 416 or 72 to 408; with the bars gone it is the whole screen
X86_EXTERN_FUNC(__imp__sub_0001A010);
X86_FUNC(sub_0001A010) {
    if (BarsOff()) {
        uint32_t x0 = kernel::R32(ctx.esp + 4), y0 = kernel::R32(ctx.esp + 8), x1 = kernel::R32(ctx.esp + 12),
                 y1 = kernel::R32(ctx.esp + 16);
        if (x0 == 0 && x1 == 640 && y0 >= 56 && y0 <= 80 && y0 + y1 >= 478 && y0 + y1 <= 482) {
            kernel::W32(ctx.esp + 8, 0);
            kernel::W32(ctx.esp + 16, 480);
        }
    }
    __imp__sub_0001A010(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001396B0);
X86_FUNC(sub_001396B0) {
    if (BarsOff()) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_001396B0(ctx, base);
}

// the shape is widescreen: 16:9 to the game, now any shape wider than 4:3
X86_FUNC(sub_0003C420) {
    kernel::Return(ctx, uint64_t(kernel::R32(game::kShapeW)) * 3 > uint64_t(kernel::R32(game::kShapeH)) * 4, 0);
}

// the device set-up and a profile's own shape write the shape: the setting's goes back in after them
X86_EXTERN_FUNC(__imp__sub_00023930);
X86_FUNC(sub_00023930) {
    __imp__sub_00023930(ctx, base);
    game::ApplyScreenShape();
}

X86_EXTERN_FUNC(__imp__sub_0019FBC0);
X86_FUNC(sub_0019FBC0) {
    __imp__sub_0019FBC0(ctx, base);
    game::ApplyScreenShape();
}

// a perspective lens built (matrix, ...): the decal passes' made the camera's
X86_EXTERN_FUNC(__imp__sub_00151CF0);
X86_FUNC(sub_00151CF0) {
    uint32_t caller = kernel::R32(ctx.esp), matrix = kernel::R32(ctx.esp + 4);
    __imp__sub_00151CF0(ctx, base);
    if (caller != game::kFromDecals1 && caller != game::kFromDecals2) return;
    uint32_t camera = kernel::R32(game::kCamera);
    if (!game::Valid(camera, 0x208) || !game::Valid(matrix, 0x18)) return;
    uint32_t lens = kernel::R32(camera + 0x204);
    if (!game::Valid(lens, 0x18)) return;
    kernel::W32(matrix, kernel::R32(lens));
    kernel::W32(matrix + 0x14, kernel::R32(lens + 0x14));
}

// A node of a model drawn, first person arms included (0x18EA90: model, node): with the weapon's angle past 100 the
// arms' two rigid shoulder roots stand out of the screen's edge, so they are not drawn. The arms entity is the
// viewed player's +0xE84 (its model +4 the header whose count is at +0 and 200-byte nodes before it); a root is
// parent 0xFF at node 0's +0x4A and, in the node, type 0 (+0x48), no parent (+0x4A) and bone 1 or 10 (+0x49), as in
// TheDoffman/timesplitters-future-perfect-pc's patch 2720. The skinned upper arms, elbows and hands are still drawn.
X86_EXTERN_FUNC(__imp__sub_0018EA90);
X86_FUNC(sub_0018EA90) {
    constexpr uint32_t kViewPlayer = 0x7B0C7C, kNodeSize = 200, kMostNodes = 128;
    uint32_t model = kernel::R32(ctx.esp + 4), node = kernel::R32(ctx.esp + 8);
    uint32_t player = kernel::R32(kViewPlayer);
    bool hide = false;
    if (node && game::Valid(player, 0xE88) && !game::FovLocked()) {
        uint32_t slot = std::min<uint32_t>(kernel::R32(player + 4), 3);
        uint32_t arms = kernel::R32(player + 0xE84);
        if (game::WeaponFov(slot) > 100 && game::Valid(arms, 8) && kernel::R32(arms + 4) == model &&
            game::Valid(model, 8) && kernel::R32(model) == arms) {
            uint32_t header = kernel::R32(model + 4), count = game::Valid(header, 4) ? kernel::R32(header) : 0;
            if (count && count <= kMostNodes && node < count && header >= count * kNodeSize) {
                uint32_t first = header - count * kNodeSize, at = first + node * kNodeSize;
                if (game::Valid(first, count * kNodeSize)) {
                    auto byte = [](uint32_t va) { return kernel::G<uint8_t>(va); };
                    hide = byte(first + 0x4A) == 255 && byte(at + 0x48) == 0 && byte(at + 0x4A) == 0 &&
                           (byte(at + 0x49) == 1 || byte(at + 0x49) == 10);
                }
            }
        }
    }
    if (hide) {
        ctx.esp += 4;  // the caller drops the arguments
        return;
    }
    __imp__sub_0018EA90(ctx, base);
}
