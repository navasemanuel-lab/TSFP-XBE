// The early Future Perfect's sniper scope, which was TimeSplitters 2's (its 0xC2CB0, the 2004 preview's too): a
// window around the aim of 0.28 of the view's height (0.45 for two players) by four thirds of that, the view outside it
// dimmed and blurred towards the edges, scopeedge rounding its corners; thin dark cross hairs, thicker towards the
// window's edges; two columns of up to ten green ticks at its bottom left, a meter wandering at random and leaning with
// the aim (its 0xC1980, crosshair type 7); a small square at its top right. The final game gives each sniper rifle its
// own reticle, drawn as its zoomed crosshair (the type in the gun's record +0x58, its function +8 in the table
// 0x4E6900, given the view's HUD and the aim): the Sniper Rifle's type 6 (0x136B40), the Sci-Fi Sniper Rifle's type 8
// (0x1364A0) and the Vintage Rifle's type 5 (0x135F30, which also draws the pistols' zoom, type 35, left as it is).
//
// TimeSplitters 2 blurred the view outside the window with a copy of the screen blended by the alpha it wrote there
// (0x1B6A50(3), blend 6); the game has no such copy at hand, so that pass is drawn as a dimming of half its alpha. The
// rest is TimeSplitters 2's to the pixel (checked against its code run in an emulator). Three or four players' scopes
// are left to the game.
#include <bit>
#include <cstdint>
#include <random>

#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kPlayer = 0x7B0C7C;   // [ ]: the view's player
constexpr uint32_t kView = 0x7E71C8;     // [ ]: left +0, right +4, half width +0x10, top +0x14, bottom +0x18,
                                         // height +0x20, half height +0x24
constexpr uint32_t kFrame = 0x4E794C;    // float: this frame's step
constexpr uint32_t kPlayers = 0x1CDAE0;  // (): the players, a view each
constexpr uint32_t kSprite = 0x80F20;    // (sprite): its texture id
constexpr uint32_t kScopeEdge = 283;
constexpr uint32_t kZMode = 0x19650;      // (mode): the one before
constexpr uint32_t kBlendMode = 0x19D70;  // (mode): 1 blends by alpha (TimeSplitters 2's 2), 2 adds (its 3)
constexpr uint32_t kSetTexture = 0x1C720, kSetSpriteTexture = 0x1ED50;  // (texture id, -1 for none)
constexpr uint32_t kFill = 0x1EFB0;  // (l, t, r, b, colour 0xRRGGBBAA, 0x7F full)
constexpr uint32_t kQuad = 0x1F090;  // (l, t, r, b, u0, v0, u1, v1 in 16ths of a texel, colour)
constexpr uint32_t kBegin = 0x18EA0;   // (3, 0xD, 0x35, vertices): triangles of {x, y, z, colour, u, v} at [kVertex]
constexpr uint32_t kVertex = 0x55F974;
constexpr uint32_t kEnd = 0x459D0;
constexpr uint32_t kCounts = 0x6B7AB0;  // three counts of what was drawn
constexpr uint32_t kVintageRifle = 5;   // the crosshair type of the Vintage Rifle zoomed

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
uint32_t U(float f) { return std::bit_cast<uint32_t>(f); }
int32_t I(uint32_t va) { return int32_t(kernel::R32(va)); }

// a rectangle shaded between the colours of its corners (top left, top right, bottom left, bottom right), each
// black at an alpha of 255 (TimeSplitters 2's 0x1B54D0 took them as 0xAARRGGBB)
void Shade(X86Context& ctx, int32_t l, int32_t t, int32_t r, int32_t b, const uint32_t (&alpha)[4]) {
    struct Corner {
        float x, y;
        uint32_t a;
    };
    const Corner tl{float(l), float(t), alpha[0]}, tr{float(r), float(t), alpha[1]}, bl{float(l), float(b), alpha[2]},
        br{float(r), float(b), alpha[3]};
    kernel::CallGuest(ctx, kBegin, {3, 0xD, 0x35, 6});
    uint32_t p = kernel::R32(kVertex);
    for (const Corner& c : {tl, tr, bl, tr, bl, br})
        for (uint32_t v : {U(c.x), U(c.y), U(0.001f), c.a / 2, U(0), U(0)}) {
            kernel::W32(p, v);
            p += 4;
        }
    kernel::W32(kVertex, p);
    kernel::CallGuest(ctx, kEnd, {});
    kernel::W32(kCounts, kernel::R32(kCounts) + 4);
    kernel::W32(kCounts + 4, kernel::R32(kCounts + 4) + 2);
    kernel::W32(kCounts + 8, kernel::R32(kCounts + 8) + 2);
}

void Fill(X86Context& ctx, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t colour) {
    kernel::CallGuest(ctx, kFill, {uint32_t(l), uint32_t(t), uint32_t(r), uint32_t(b), colour});
}

// TimeSplitters 2's meter (0xC1980, type 7): each column eases towards a level, 0.05 a frame for the first, 0.025
// for the second, and on reaching it picks another: half way, plus up to 0.15 either side at random, plus 0.8 of the
// aim's lean (three times its offset across for the first, up for the second), held within 0 and 1
struct Meter {
    float level[2], towards[2];
};
Meter g_meters[4];

void StepMeter(Meter& m, uint32_t player) {
    static std::minstd_rand random;
    float frame = F(kFrame), lean[2] = {F(player + 0xF6C) * 3.0f, -F(player + 0xF70) * 3.0f};
    for (int i = 0; i < 2; i++) {
        float rate = i ? 0.025f : 0.05f;
        if (m.level[i] == m.towards[i]) {
            float r = float(random() - random.min()) / float(random.max() - random.min());
            m.towards[i] = (r - 0.5f) * 0.3f + lean[i] * 0.8f + 0.5f;
            m.towards[i] = m.towards[i] < 0.0f ? 0.0f : m.towards[i] > 1.0f ? 1.0f : m.towards[i];
        }
        if (m.level[i] < m.towards[i]) {
            m.level[i] += frame * rate;
            if (m.level[i] > m.towards[i]) m.level[i] = m.towards[i];
        } else if (m.level[i] > m.towards[i]) {
            m.level[i] -= frame * rate;
            if (m.level[i] < m.towards[i]) m.level[i] = m.towards[i];
        }
    }
}

// TimeSplitters 2's scope around the aim (x, y), false when it is one the game draws
bool DrawTs2Scope(X86Context& ctx, int32_t x, int32_t y) {
    uint32_t player = kernel::R32(kPlayer), view = kernel::R32(kView);
    if (!player || !view) return false;
    uint32_t players = kernel::CallGuest(ctx, kPlayers, {});
    if (players > 2) return false;
    Meter& meter = g_meters[kernel::R32(player + 4) & 3];
    StepMeter(meter, player);

    int32_t L = I(view), R = I(view + 4), T = I(view + 0x14), B = I(view + 0x18);
    // the window (TimeSplitters 2 worked these out on the x87, at more than a float's precision)
    double k = double(players == 2 ? 0.45f : 0.28f) * double(I(view + 0x20));
    int32_t wx = int32_t(k * double(1.3333334f)), wy = int32_t(k);
    int32_t rx = int32_t(double(wx) * double(0.8f)), ry = int32_t(double(wy) * double(0.8f));
    int32_t l = x - wx, r = x + wx, t = y - wy, b = y + wy;

    uint32_t z = kernel::CallGuest(ctx, kZMode, {0});
    kernel::CallGuest(ctx, kBlendMode, {1});
    uint32_t edge = kernel::CallGuest(ctx, kSprite, {kScopeEdge});
    // the view around the window: dimmed, then (for the blur) dimmed more towards the view's edges
    for (int pass = 0; pass < 2; pass++) {
        uint32_t a = pass ? 0x6E / 2 : 0x1E, c = pass ? 0x2B / 2 : 0x1E;  // at the view's edge, at the window's
        kernel::CallGuest(ctx, kSetTexture, {0xFFFFFFFFu});
        Shade(ctx, L, T, l, t, {a, a, a, c});
        Shade(ctx, r, T, R, t, {a, a, c, a});
        Shade(ctx, L, t, l, b, {a, c, a, c});
        Shade(ctx, l, T, r, t, {a, a, c, c});
        Shade(ctx, r, t, R, b, {c, a, c, a});
        Shade(ctx, l, b, r, B, {c, c, a, a});
        Shade(ctx, L, b, l, B, {a, c, a, a});
        Shade(ctx, r, b, R, B, {c, a, a, a});
        if (edge != 0xFFFFFFFF) {
            uint32_t q = pass ? 0x2B / 2 : 0x1E;
            kernel::CallGuest(ctx, kSetSpriteTexture, {edge});
            auto corner = [&](int32_t cl, int32_t ct, uint32_t u0, uint32_t v0, uint32_t u1, uint32_t v1) {
                kernel::CallGuest(ctx, kQuad, {uint32_t(cl), uint32_t(ct), uint32_t(cl + 25), uint32_t(ct + 25), u0,
                                               v0, u1, v1, q});
            };
            corner(l, t, 2, 2, 1024, 1024);
            corner(l, b - 25, 2, 1024, 1024, 2);
            corner(r - 25, t, 1024, 2, 2, 1024);
            corner(r - 25, b - 25, 1024, 1024, 2, 2);
        }
    }
    kernel::CallGuest(ctx, kSetTexture, {0xFFFFFFFFu});
    // the cross hairs
    Fill(ctx, x - 1, t, x + 1, b, 15);
    Fill(ctx, l, y - 1, r, y + 2, 15);
    Fill(ctx, l, y - 3, x - rx, y + 4, 15);
    Fill(ctx, x + rx, y - 3, r, y + 4, 15);
    Fill(ctx, x - 3, t, x + 4, y - ry, 15);
    Fill(ctx, x - 3, b, x + 4, y + ry, 15);
    // the meter: ticks of two pixels every four, up from the bottom left, greener up the column
    for (uint32_t i = 0; i < 2; i++) {
        int32_t lit = int32_t(meter.level[i] * 10.0f), left = l + 15 + 15 * int32_t(i);
        for (uint32_t n = 0; n < 10; n++) {
            if (int32_t(n) > lit) continue;
            uint32_t red = i ? 0x1B0000 + n * 0xB0000 : 0x320000 + n * 0x40000;
            uint32_t top = i ? 0x28000000 + n * 0x6000000 : 0x14000000 + n * 0xD000000;
            uint32_t colour = (red & 0xFF0000) | (top & 0xFF000050) | ((0x1400 + n * 0x200) & 0xFF00) | 0x50;
            Fill(ctx, left, b - 12 - 4 * int32_t(n), left + 10, b - 10 - 4 * int32_t(n), colour);
        }
    }
    kernel::CallGuest(ctx, kBlendMode, {2});
    Fill(ctx, r - 25, t + 15, r - 15, t + 25, 0x1414327F);
    kernel::CallGuest(ctx, kZMode, {z});
    return true;
}

// a zoomed crosshair's function, given (HUD, aim x, aim y as floats): TimeSplitters 2's scope in the early style
bool EarlyScope(X86Context& ctx) {
    if (game::Opt().hud_style != 1) return false;
    int32_t x = int32_t(F(ctx.esp + 8)), y = int32_t(F(ctx.esp + 12));
    if (!DrawTs2Scope(ctx, x, y)) return false;
    static uint32_t logged;
    uint32_t type = kernel::R32(kernel::R32(ctx.esp + 4));
    if (!(logged & 1u << (type & 31))) {
        LOGR("hud: scope of crosshair %u drawn as TimeSplitters 2's", type);
        logged |= 1u << (type & 31);
    }
    kernel::Return(ctx, 0, 0);
    return true;
}

}  // namespace

// the Sniper Rifle's zoomed crosshair (type 6)
X86_EXTERN_FUNC(__imp__sub_00136B40);
X86_FUNC(sub_00136B40) {
    if (!EarlyScope(ctx)) __imp__sub_00136B40(ctx, base);
}

// the Sci-Fi Sniper Rifle's (type 8)
X86_EXTERN_FUNC(__imp__sub_001364A0);
X86_FUNC(sub_001364A0) {
    if (!EarlyScope(ctx)) __imp__sub_001364A0(ctx, base);
}

// the Vintage Rifle's (type 5) and the pistols' zoom (type 35)
X86_EXTERN_FUNC(__imp__sub_00135F30);
X86_FUNC(sub_00135F30) {
    uint32_t hud = kernel::R32(ctx.esp + 4);
    if (hud && kernel::R32(hud) == kVintageRifle && EarlyScope(ctx)) return;
    __imp__sub_00135F30(ctx, base);
}
