// The HUD of the early Future Perfect, which was TimeSplitters 2's (the November 2004 preview keeps it): the health
// bars of the others in a level (an ally, a boss, a thing to protect) in the top right corner, one under the other,
// where the final game centres them at the top (0x139AD0, 0x139E20) or the bottom (0x139740).
//
// Those bars are drawn by 0x130700 and 0x130400, each (left, top, right, bottom, ...) in screen pixels, from the
// view's rectangle ([0x7E71C8]: left +0, right +4, top +0x14, bottom +0x18) less its safe margins (0x7A6140 left,
// 0x7A6144 top, 0x7A6148 right, 0x7A614C bottom). The HUD of each player's view starts at 0x14CC80. In the early
// style they are drawn here instead, with the game's own 2D calls: its name beside a bar of coloured rectangles with no
// texture.
//
// The arcs of health and armour either side of the crosshair, shown a while after either changes, are TimeSplitters
// 2's in both games: the same state (a character's [+0x268]: 20 segments' on flags at +0 and +0x50, their fades at
// +0xA0 and +0xF0, the arcs' fade at +0x140, 0x13A2C0 updating it as TimeSplitters 2's 0xC4D60 does) drawn by
// 0x13AB60, to which the final game gave other colours, an outline and another size. In the early style
// TimeSplitters 2's drawing (its 0xC4F10) is done instead, with its sizes and colours.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kView = 0x7E71C8;
constexpr uint32_t kMarginLeft = 0x7A6140, kMarginTop = 0x7A6144, kMarginRight = 0x7A6148;
// the game's 2D calls
constexpr uint32_t kSetTexture = 0x1C720;  // (texture id)
// (left, top, right, bottom, colour 0xRRGGBBAA with 0x7F full): a rectangle of one colour, two triangles, with no
// texture set (0x1AFA0 beside it, of the same arguments, is a line from corner to corner)
constexpr uint32_t kRectangle = 0x1A2E0;
constexpr uint32_t kTextWidth = 0x405C0;    // (text): its width, on the x87 stack
constexpr uint32_t kPrint = 0x41E30;        // (x, y, text, colour 0xRRGGBBAA, shadow colour)
constexpr uint32_t kTextStyle = 0x3FA10;    // (style): the one before
constexpr uint32_t kTextScale = 0x3F9C0;    // (x, y, ?)
constexpr uint32_t kLabelStyle = 16;  // Arial Bold, small
// TimeSplitters 2's bar (its default.xbe, 0xC8F00 from 0xC9A31): from 70 to 95 hundredths of the view's width, 20
// lines under its top and 14 high of 480, in a dark frame 2 wider each side; its name 10 to the left in style 16;
// 20 segments shaded yellow to red from its table (0x2CBEE4, red green blue of 127, times 1.4), the health left at
// alpha 0x7F at the right, what was lost at 0x46 at the left
constexpr float kLeft = 0.70f, kRight = 0.95f, kTop = 20.0f / 480, kHeight = 14.0f / 480, kGap = 10.0f / 480;
constexpr float kFrame = 2, kLabelGap = 10;
constexpr int kSegments = 20;
constexpr uint8_t kGreen[kSegments + 1] = {127, 127, 127, 119, 110, 100, 90, 82, 75, 67, 60,
                                           52,  45,  37,  30,  25,  20,  20, 10, 10, 0};

// the arcs: the game's calls, as 0x13AB60 makes them
constexpr uint32_t kPlayers = 0x1CDAE0;   // (): the players, a view each
constexpr uint32_t kSprite = 0x80F20;     // (sprite): its texture id
constexpr uint32_t kZMode = 0x19650;      // (mode): the one before; 0 tests nothing and writes nothing
constexpr uint32_t kBlendMode = 0x19D70;  // (mode): 2 adds
constexpr uint32_t kBegin = 0x18EA0;      // (3, 0xD, 0x35, vertices): triangles of {x, y, z, colour, u, v} at [kVertex]
constexpr uint32_t kVertex = 0x55F974;
constexpr uint32_t kEnd = 0x459D0;
constexpr uint32_t kCounts = 0x6B7AB0;    // three counts of what was drawn
constexpr uint32_t kArcBar = 803, kArcGlow = 802;  // sprites arc_bar and arc_glow
// TimeSplitters 2's segments (0x2CBED8 health, 0x2CC080 armour): {the part of the most it shows, where it starts down
// the arc, red, green, blue of 127}, the last one where the arc ends
struct Segment {
    float part, at;
    int32_t r, g, b;
};
constexpr Segment kHealth[kSegments + 1] = {
    {1.00f, 0.00f, 127, 127, 0}, {0.97f, 0.15f, 127, 127, 0}, {0.94f, 0.20f, 127, 127, 0}, {0.90f, 0.25f, 127, 119, 0},
    {0.86f, 0.30f, 127, 110, 0}, {0.82f, 0.35f, 127, 100, 0}, {0.78f, 0.40f, 127, 90, 0},  {0.74f, 0.45f, 127, 82, 0},
    {0.70f, 0.50f, 127, 75, 0},  {0.66f, 0.55f, 127, 67, 0},  {0.62f, 0.60f, 127, 60, 0},  {0.57f, 0.64f, 127, 52, 0},
    {0.53f, 0.68f, 127, 45, 0},  {0.48f, 0.73f, 127, 37, 0},  {0.43f, 0.75f, 127, 30, 0},  {0.37f, 0.78f, 127, 25, 0},
    {0.30f, 0.81f, 127, 20, 0},  {0.20f, 0.84f, 127, 20, 0},  {0.10f, 0.87f, 127, 10, 0},  {0.00f, 0.90f, 127, 10, 0},
    {0.00f, 1.00f, 127, 0, 0}};
constexpr Segment kArmour[kSegments + 1] = {
    {1.00f, 0.00f, 54, 127, 0},  {0.95f, 0.05f, 52, 120, 5},  {0.90f, 0.10f, 51, 115, 10}, {0.85f, 0.15f, 49, 107, 20},
    {0.80f, 0.20f, 48, 100, 30}, {0.75f, 0.25f, 46, 95, 35},  {0.70f, 0.30f, 44, 90, 40},  {0.65f, 0.35f, 43, 82, 45},
    {0.60f, 0.40f, 42, 75, 50},  {0.55f, 0.45f, 41, 65, 57},  {0.50f, 0.50f, 40, 55, 65},  {0.45f, 0.55f, 39, 53, 72},
    {0.40f, 0.60f, 38, 50, 80},  {0.35f, 0.65f, 37, 47, 85},  {0.30f, 0.70f, 36, 45, 90},  {0.25f, 0.75f, 35, 43, 95},
    {0.20f, 0.80f, 34, 40, 100}, {0.15f, 0.85f, 33, 37, 105}, {0.10f, 0.90f, 32, 35, 110}, {0.05f, 0.95f, 31, 33, 115},
    {0.00f, 1.00f, 30, 30, 120}};

uint32_t g_bars = 0;  // the bars drawn in this view's HUD so far

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
uint32_t U(float f) { return std::bit_cast<uint32_t>(f); }

// a rectangle of one colour, 0xRRGGBBAA with 0x7F full
void Fill(X86Context& ctx, float l, float t, float r, float b, uint32_t c) {
    kernel::CallGuest(ctx, kRectangle, {U(l), U(t), U(r), U(b), c});
}

// a byte of TimeSplitters 2's colour table, times 1.4 (0x7F is full, more is brighter)
uint32_t Bright(uint32_t v) { return std::min<uint32_t>(255, uint32_t(float(v) * 1.4f + 0.5f)); }

// a health bar as TimeSplitters 2's, which the early Future Perfect kept: its name to the left, a dark frame, 20
// segments yellow to red drained from the left, in the view's top right corner under the bars before it. health 0
// to 1
bool DrawTs2Bar(X86Context& ctx, float health, uint32_t text) {
    uint32_t view = kernel::R32(kView);
    if (!view) return false;
    float view_left = float(int32_t(kernel::R32(view)) + int32_t(kernel::R32(kMarginLeft)));
    float view_right = float(int32_t(kernel::R32(view + 4)) - int32_t(kernel::R32(kMarginRight)));
    float view_top = float(int32_t(kernel::R32(view + 0x14)) + int32_t(kernel::R32(kMarginTop)));
    float h = float(int32_t(kernel::R32(view + 0x18)) - int32_t(kernel::R32(view + 0x14)));
    float w = view_right - view_left, bh = h * kHeight;
    float left = view_left + w * kLeft, right = view_left + w * kRight;
    float top = view_top + h * kTop + float(g_bars++) * (bh + h * kGap), bottom = top + bh;
    float lost = left + (right - left) * (1 - std::clamp(health, 0.0f, 1.0f));  // the lit part from here on

    kernel::CallGuest(ctx, kSetTexture, {0xFFFFFFFFu});  // none
    Fill(ctx, left - kFrame, top - kFrame, right + kFrame, bottom + kFrame, 0x28282864);
    // each segment shaded from its colour to the next's, in slices of one colour, a slice split where the lit part
    // starts
    constexpr int kSlices = 4;
    float seg = (right - left) / kSegments;
    for (int i = 0; i < kSegments; i++) {
        for (int s = 0; s < kSlices; s++) {
            float k = (float(s) + 0.5f) / kSlices;
            uint32_t g = Bright(uint32_t(float(kGreen[i]) + (float(kGreen[i + 1]) - float(kGreen[i])) * k));
            uint32_t colour = Bright(127) << 24 | g << 16;
            float l = left + seg * (float(i) + float(s) / kSlices), z = left + seg * (float(i) + float(s + 1) / kSlices);
            float m = std::clamp(lost, l, z);
            if (m > l) Fill(ctx, l, top, m, bottom, colour | 0x46);
            if (z > m) Fill(ctx, m, top, z, bottom, colour | 0x7F);
        }
    }

    if (text) {
        uint32_t was = kernel::CallGuest(ctx, kTextStyle, {kLabelStyle});
        kernel::CallGuest(ctx, kTextScale, {U(1), U(1), U(1)});
        kernel::CallGuest(ctx, kTextWidth, {text});
        float tw = float(ctx.st[ctx.top]);
        x87_pop(ctx);
        float x = left - kFrame - kLabelGap - tw, y = top + bh * 0.5f - bh * 0.62f;
        kernel::CallGuest(ctx, kPrint, {U(x), U(y), text, 0x7F7F7F6E, 0x00000050});
        kernel::CallGuest(ctx, kTextStyle, {was});
    }
    return true;
}

// a segment's brightness, 0 to 1: its flag, a change of the last 0.7 seconds fading in or out (TimeSplitters 2's
// 0xC4EA0)
float Lit(uint32_t state, int side, int i) {
    float t = F(state + 0xA0 + uint32_t(side) * 0x50 + uint32_t(i) * 4);
    bool on = kernel::R32(state + uint32_t(side) * 0x50 + uint32_t(i) * 4) != 0;
    if (t > 0) {
        float k = std::min(1.0f, t * 1.4285714f);
        return on ? 1 - k : k;
    }
    return on ? 1.0f : 0.0f;
}

// TimeSplitters 2's arcs (0xC4F10) of a character's state, centred on (cx, cy): the bars, then their glow, of 20
// segments each, the health's left of the centre and the armour's right of it, mirrored. on, off: the alpha of a
// segment lit and unlit, the glow's at full; scale: of TimeSplitters 2's size; apart: how far each arc is moved out
// from the centre
void DrawTs2Arcs(X86Context& ctx, uint32_t state, float cx, float cy, uint32_t on, uint32_t off, float scale = 1,
                 float apart = 0) {
    uint32_t players = kernel::CallGuest(ctx, kPlayers, {});
    float w = float(players >= 3 ? 300 : 562) * (players > 2 ? 1.08f : 1.2f) * 0.5f * scale;
    float h = float(players == 1 ? 340 : 220) * 0.5f * scale, top = cy - h;
    uint32_t z = kernel::CallGuest(ctx, kZMode, {0});
    for (int pass = 0; pass < 2; pass++) {
        kernel::CallGuest(ctx, kSetTexture, {kernel::CallGuest(ctx, kSprite, {pass ? kArcGlow : kArcBar})});
        kernel::CallGuest(ctx, kBlendMode, {2});
        for (int side = 0; side < 2; side++) {
            const Segment* t = side ? kArmour : kHealth;
            int mid = side ? 10 : 8;  // where the texture, the arc's upper half, turns over for the lower
            float shift = side ? apart : -apart;
            float xl = (side ? cx : cx - w) + shift, xr = (side ? cx + w : cx) + shift, ul = side ? 1.0f : 0.0f;
            for (int i = 0; i < kSegments; i++) {
                const Segment &e = t[i], &n = t[i + 1];
                float y0 = top + e.at * 2 * h, y1 = top + n.at * 2 * h, vt, vb;
                if (i < mid) {
                    vt = 1 - 2 * e.at, vb = 1 - 2 * n.at;
                    if (i == 0) vt -= 0.01f;
                    else if (i == mid - 1) vb += 0.01f;
                } else {
                    vt = (e.at - 0.5f) * 2, vb = (n.at - 0.5f) * 2;
                    if (i == mid) vt += 0.01f;
                }
                // the bar: unlit at three tenths of the colour, at the alpha of its flag; the glow: the colour, at
                // the alpha of the segment's brightness or half its next's, whichever more
                float lit = Lit(state, side, i);
                auto colour = [&](const Segment& c, uint32_t alpha) {
                    uint32_t out = 0;
                    for (int32_t v : {c.r, c.g, c.b}) {
                        if (!pass) {
                            int32_t dim = int32_t(float(v) * 0.3f);
                            v = int32_t(float(v - dim) * lit + float(dim));
                        }
                        out = out << 8 | uint32_t(v & 0xFF);
                    }
                    return out << 8 | (alpha & 0xFF);
                };
                uint32_t at, ab;
                if (!pass) {
                    at = ab = kernel::R32(state + uint32_t(side) * 0x50 + uint32_t(i) * 4) ? on : off;
                } else {
                    float a = i < kSegments - 1 ? std::max(lit, 0.5f * Lit(state, side, i + 1)) : lit;
                    at = uint32_t(int32_t(a * float(on)));
                    ab = at;
                    if (i < kSegments - 1) {
                        float b = Lit(state, side, i + 1);
                        if (i < kSegments - 2) b = std::max(b, 0.5f * Lit(state, side, i + 2));
                        ab = uint32_t(int32_t(b * float(on)));
                    }
                }
                uint32_t ct = colour(e, at), cb = colour(n, ab);
                // two triangles: the top left, top right and bottom left corners, then top right, bottom left and
                // bottom right
                struct Corner {
                    float x, y, u, v;
                    uint32_t c;
                };
                const Corner quad[6] = {{xl, y0, ul, vt, ct},     {xr, y0, 1 - ul, vt, ct}, {xl, y1, ul, vb, cb},
                                        {xr, y0, 1 - ul, vt, ct}, {xl, y1, ul, vb, cb},     {xr, y1, 1 - ul, vb, cb}};
                kernel::CallGuest(ctx, kBegin, {3, 0xD, 0x35, 6});
                uint32_t p = kernel::R32(kVertex);
                for (const Corner& c : quad) {
                    for (uint32_t v : {U(c.x), U(c.y), U(0.001f), c.c, U(c.u), U(c.v)}) {
                        kernel::W32(p, v);
                        p += 4;
                    }
                }
                kernel::W32(kVertex, p);
                kernel::CallGuest(ctx, kEnd, {});
                kernel::W32(kCounts, kernel::R32(kCounts) + 4);
                kernel::W32(kCounts + 4, kernel::R32(kCounts + 4) + 2);
                kernel::W32(kCounts + 8, kernel::R32(kCounts + 8) + 2);
            }
        }
    }
    kernel::CallGuest(ctx, kZMode, {z});
}

}  // namespace

namespace game {

// the arcs of health and armour (state, x, y, scale, alphas...): TimeSplitters 2's in the early style. From the view's
// HUD (0x14D3A0) while their fade [state+0x140] lasts, its alphas of the fade as its 0xC9250 makes them; from the pause
// menus (0x2E5750, Objectives among them) at full, at the final game's scale there and where its gauges are
// (objectives.cpp): one size and place on every page. Called by objectives.cpp's hook of
// 0x13AB60; true when drawn and returned from
bool DrawEarlyArcs(X86Context& ctx) {
    constexpr uint32_t kFromHud = 0x14D46D, kFromPause = 0x2E58C9;
    uint32_t from = kernel::R32(ctx.esp), state = kernel::R32(ctx.esp + 4), view = kernel::R32(kView);
    if (Opt().hud_style == 2 && from == kFromHud) {  // TimeSplitters 1's bars stand for them
        kernel::Return(ctx, 0, 0);
        return true;
    }
    if (Opt().hud_style != 1 || (from != kFromHud && from != kFromPause) || !state || !view) return false;
    float x = F(ctx.esp + 8), y = F(ctx.esp + 12);
    float left = float(int32_t(kernel::R32(view)));
    float cx = float(int32_t(kernel::R32(view + 8))) + x - left;
    float cy = float(int32_t(kernel::R32(view + 0x1C))) + y - float(int32_t(kernel::R32(view + 0x14)));
    if (from == kFromPause) {
        float scale = F(ctx.esp + 16);
        if (!(scale > 0)) scale = 1;
        uint32_t on = kernel::R32(ctx.esp + 20) & 0xFF;
        DrawTs2Arcs(ctx, state, cx, cy, on, on * 50 / 127, scale, 0.0f);
    } else {
        float k = std::min(1.0f, std::sqrt(std::max(F(state + 0x140), 0.0f) * 0.5f) * 1.2f);
        DrawTs2Arcs(ctx, state, cx, cy, uint32_t(int32_t(k * 127)), uint32_t(int32_t(k * 50)));
    }
    static bool logged[2] = {};
    if (!logged[from == kFromPause]) {
        logged[from == kFromPause] = true;
        LOGR("hud: TimeSplitters 2's arcs%s, centred at %.0f %.0f", from == kFromPause ? " in a pause menu" : "", cx, cy);
    }
    kernel::Return(ctx, 0, 0);
    return true;
}

}  // namespace game

// Print(x, y, text, colour, shadow): the messages over the view of a kind 3 (0x14A280: "First kill: Monkey!" and the
// like) in TimeSplitters 2's colours in the early style, its pale green on a dark green shadow (its 0xBC1C5), where the
// final game's are white on a dark blue
X86_EXTERN_FUNC(__imp__sub_00041E30);
X86_FUNC(sub_00041E30) {
    constexpr uint32_t kFromMessage = 0x14A539;
    constexpr uint32_t kWhite = 0x7F7F7F7F, kTs2Colour = 0x64786464, kTs2Shadow = 0x00280060;
    if (kernel::R32(ctx.esp) == kFromMessage && game::Opt().hud_style == 1 && kernel::R32(ctx.esp + 16) == kWhite) {
        kernel::W32(ctx.esp + 16, kTs2Colour);
        kernel::W32(ctx.esp + 20, kTs2Shadow);
    }
    __imp__sub_00041E30(ctx, base);
}

// The teams' table (0x4D1E28, 0x14 each, team 0 none): +0 the team's tint 0xRRGGBBAA, +4 its icon, FP's skull
// newHud_skull (0xE8) for red, blue, yellow and green (teams 1 to 4). TimeSplitters 2's own icons are those teams'
// blueteam, greenteam, redteam and yellowteam (sprites 332 to 335), in their own colours.
namespace {
constexpr uint32_t kTeams = 0x4D1E28, kTeamSize = 0x14;
constexpr uint32_t kTeamIcons[5] = {0, 334, 332, 335, 333};
constexpr uint32_t kSetSprite = 0x1ED50;  // (texture id)
uint32_t g_marker_team = 0;               // the team of the character whose marker is drawn next

uint32_t TeamIcon(uint32_t team) { return team >= 1 && team <= 4 ? kTeamIcons[team] : 0; }
}  // namespace

// a fade (0x11F780) of the character's +0xC68, which the team markers' drawing (0x13C560) asks for just before it
// reads the character's team at +0x84
X86_EXTERN_FUNC(__imp__sub_0011F780);
X86_FUNC(sub_0011F780) {
    if (kernel::R32(ctx.esp) == 0x13CC72) g_marker_team = kernel::R32(kernel::R32(ctx.esp + 4) - 0xC68 + 0x84);
    __imp__sub_0011F780(ctx, base);
}

// The teams' icons in the character select (0x2C0A70, a player's, and 0x2C4D20, the choice of a team): the teams'
// table's icon tinted with its colour. In the early style TimeSplitters 2's icon of the team, untinted, the table's
// entries swapped while they are drawn
namespace game {
void WithTs2Teams(X86Context& ctx, uint8_t* base, void (*draw)(X86Context&, uint8_t*)) {
    if (game::Opt().hud_style != 1) {
        draw(ctx, base);
        return;
    }
    uint32_t saved[5][2];
    for (uint32_t t = 1; t <= 4; t++) {
        uint32_t e = kTeams + t * kTeamSize;
        saved[t][0] = kernel::R32(e);
        saved[t][1] = kernel::R32(e + 4);
        kernel::W32(e, 0x7F7F7F00 | (saved[t][0] & 0xFF));
        kernel::W32(e + 4, kTeamIcons[t]);
    }
    draw(ctx, base);
    for (uint32_t t = 1; t <= 4; t++) {
        kernel::W32(kTeams + t * kTeamSize, saved[t][0]);
        kernel::W32(kTeams + t * kTeamSize + 4, saved[t][1]);
    }
}
}  // namespace game

X86_EXTERN_FUNC(__imp__sub_002C0A70);
X86_FUNC(sub_002C0A70) { game::WithTs2Teams(ctx, base, __imp__sub_002C0A70); }

// The marker over a character's head in team games (0x13C560): FP's split skull in two mirrored halves tinted with the
// team's colour (0x1BA70 from 0x13CF75, the left half, and 0x13D049, the right one or its talking variant). In the
// early style TimeSplitters 2's icon of the character's team is drawn instead, whole and in its own colours, a square
// on the halves' seam
namespace game {
// called first by the quad's hook (menu_mouse.cpp), on (left, top, right, bottom, u0, v0, u1, v1, four colours): true
// when the quad is to be left out, its args otherwise changed to the icon's
bool EarlyTeamMarker(X86Context& ctx) {
    constexpr uint32_t kLeftHalf = 0x13CF7A, kRightHalf = 0x13D04E, kIconTexels = 64 * 16;
    uint32_t ret = kernel::R32(ctx.esp), a = ctx.esp + 4;
    if (Opt().hud_style != 1 || (ret != kLeftHalf && ret != kRightHalf)) return false;
    uint32_t sprite = TeamIcon(g_marker_team);
    if (!sprite) return false;
    if (ret == kLeftHalf) return true;  // the right half draws the whole icon
    g_marker_team = 0;                  // the next character's comes with its own fade
    float l = F(a), t = F(a + 4), b = F(a + 12), size = b - t;
    if (!(size > 0)) return false;
    kernel::CallGuest(ctx, kSetSprite, {kernel::CallGuest(ctx, kSprite, {sprite})});
    for (auto [at, v] : {std::pair{0u, U(l - size * 0.5f)}, {8u, U(l + size * 0.5f)}, {16u, 0u}, {20u, 0u},
                         {24u, kIconTexels}, {28u, kIconTexels}})
        kernel::W32(a + at, v);
    for (uint32_t i = 0; i < 4; i++) kernel::W32(a + 32 + i * 4, 0x7F7F7F00 | (kernel::R32(a + 32 + i * 4) & 0xFF));
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGR("hud: TimeSplitters 2's team icon %u over a head (team %u)", sprite, g_marker_team);
    }
    return false;
}
}  // namespace game

// The HUD's boxes and the radar in TimeSplitters 2's blue. FP's rank (0x12EC20), time, points and the like are its generic box (0x12E3F0): a dark corner
// gradient (0x12CC70 from 0x12E643) under brackets of newHud_box, six quads (0x1BA70). TimeSplitters 2's (its 0xBD183
// by 0xBCE20, the same for its time, points and the like) is hudback, a ring of four ticks, stretched over the whole
// box, tinted 0x3C417F or its player's team's colour in a team game, its clock (0xBD250: time taken or left) grey
// 0x505050: here every box's, FP's time taken or left (0x12ED10, from 0x12EDC0, and 0x14F5C7) grey. Its radar's ring,
// centre and pulse are tinted the same (0xBFD10, 0xBFF58, 0xC038C), where FP's are white: the ring hudradar and the
// pulse by 0x1B7B0 (from 0x13427A and 0x134C96, colour 0xRRGGBBAA), the centre by 0x167B0 (from 0x1346A1, its colour
// the third argument, 0xAABBGGRR)
namespace {
constexpr uint32_t kBoxPieces[6] = {0x12E67D, 0x12E6A3, 0x12E6C2, 0x12E6E8, 0x12E714, 0x12E735};  // the last always
constexpr uint32_t kHudBack = 249, kHudBackTexels = 64 * 16;
constexpr uint32_t kTs2Blue = 0x3C417F00;  // 0xRRGGBBAA, alpha left out
constexpr float kRankAbove = 0.5f, kRankAside = 0.1f;  // of the brackets' height and width: the label above them inside
bool g_rank_box = false, g_grey_box = false;
constexpr uint32_t kGreyBoxes[2] = {0x12EDC5, 0x14F5CC};  // the box's returns to its clocks
constexpr uint32_t kTs2Grey = 0x50505000;
float g_box[4];  // the brackets' union so far
uint32_t g_box_pieces = 0;

// white, the final game's, kept to its alpha: tinted TimeSplitters 2's blue
uint32_t Blue(uint32_t c) { return (c & 0xFFFFFF00) == 0x7F7F7F00 ? kTs2Blue | (c & 0xFF) : c; }

// the rank's tint: in a team game ([0x7DE458] bit 0) the view's player's team's (its character [[body]+0x7C]+0x84,
// teams 1 to 4 red, blue, yellow, green) from TimeSplitters 2's table (0x2BD158), as its 0xBD192; the blue otherwise.
// A challenge (mode [0x78AD20] 7) is played as a team game too, its player on yellow (Behead the Undead): grey there
constexpr uint32_t kGameFlags = 0x7DE458, kViewPlayer = 0x7B0C7C, kPlayMode = 0x78AD20, kChallenge = 7;
constexpr uint32_t kTs2Teams[5] = {kTs2Blue, 0x7F101000, 0x203F7F00, 0x7F7F0000, 0x107F1000};
uint32_t RankTint() {
    if (kernel::R32(kPlayMode) == kChallenge) return kTs2Grey;
    uint32_t player = kernel::R32(kViewPlayer), body = player ? kernel::R32(player + 0x14) : 0;
    uint32_t chr = body ? kernel::R32(body + 0x7C) : 0;
    if (!(kernel::R32(kGameFlags) & 1) || !chr) return kTs2Blue;
    uint32_t team = kernel::R32(chr + 0x84);
    return team >= 1 && team <= 4 ? kTs2Teams[team] : kTs2Blue;
}
}  // namespace

namespace game {
// called by the quad's hook (menu_mouse.cpp), on (left, top, right, bottom, u0, v0, u1, v1, four colours): true when
// the quad is to be left out. A box's brackets are gathered and the last of them drawn as hudback over them all
bool EarlyRankBox(X86Context& ctx) {
    uint32_t ret = kernel::R32(ctx.esp), a = ctx.esp + 4;
    if (!g_rank_box || std::find(std::begin(kBoxPieces), std::end(kBoxPieces), ret) == std::end(kBoxPieces)) return false;
    float l = std::min(F(a), F(a + 8)), r = std::max(F(a), F(a + 8));
    float t = std::min(F(a + 4), F(a + 12)), b = std::max(F(a + 4), F(a + 12));
    if (!g_box_pieces++) {
        g_box[0] = l, g_box[1] = t, g_box[2] = r, g_box[3] = b;
    } else {
        g_box[0] = std::min(g_box[0], l), g_box[1] = std::min(g_box[1], t);
        g_box[2] = std::max(g_box[2], r), g_box[3] = std::max(g_box[3], b);
    }
    if (ret != kBoxPieces[5]) return true;
    float w = g_box[2] - g_box[0], h = g_box[3] - g_box[1];
    kernel::CallGuest(ctx, kSetSprite, {kernel::CallGuest(ctx, kSprite, {kHudBack})});
    for (auto [at, v] : {std::pair{0u, U(g_box[0] - w * kRankAside)}, {4u, U(g_box[1] - h * kRankAbove)},
                         {8u, U(g_box[2] + w * kRankAside)}, {12u, U(g_box[3])}, {16u, 0u}, {20u, 0u},
                         {24u, kHudBackTexels}, {28u, kHudBackTexels}})
        kernel::W32(a + at, v);
    uint32_t tint = g_grey_box ? kTs2Grey : RankTint();
    for (uint32_t i = 0; i < 4; i++) kernel::W32(a + 32 + i * 4, tint | (kernel::R32(a + 32 + i * 4) & 0xFF));
    static uint32_t logged = 0;
    if (logged != tint) {
        logged = tint;
        LOGR("hud: TimeSplitters 2's boxes, hudback over %.0f %.0f %.0f %.0f tinted %06X", g_box[0], g_box[1], g_box[2],
             g_box[3], tint >> 8);
    }
    return false;
}
}  // namespace game

// the generic box (x, y, ...): its brackets gathered
X86_EXTERN_FUNC(__imp__sub_0012E3F0);
X86_FUNC(sub_0012E3F0) {
    bool was = g_rank_box, was_grey = g_grey_box;
    uint32_t from = kernel::R32(ctx.esp);
    g_rank_box = game::Opt().hud_style == 1;
    g_grey_box = std::find(std::begin(kGreyBoxes), std::end(kGreyBoxes), from) != std::end(kGreyBoxes);
    g_box_pieces = 0;
    __imp__sub_0012E3F0(ctx, base);
    g_rank_box = was, g_grey_box = was_grey;
}

// the boxes' corner gradient: none under hudback
X86_EXTERN_FUNC(__imp__sub_0012CC70);
X86_FUNC(sub_0012CC70) {
    if (g_rank_box && kernel::R32(ctx.esp) == 0x12E648) return kernel::Return(ctx, 0, 0);
    __imp__sub_0012CC70(ctx, base);
}

// a triangle fan of one colour (?, ?, colour 0xAABBGGRR, ...): the radar's centre
X86_EXTERN_FUNC(__imp__sub_000167B0);
X86_FUNC(sub_000167B0) {
    if (kernel::R32(ctx.esp) == 0x1346A6 && game::Opt().hud_style == 1) {
        uint32_t c = kernel::R32(ctx.esp + 12);
        if ((c & 0xFFFFFF) == 0x7F7F7F) kernel::W32(ctx.esp + 12, (c & 0xFF000000) | 0x7F413C);
    }
    __imp__sub_000167B0(ctx, base);
}

// TimeSplitters 1's health and armour (the HUD style 2): two long bars across the top left corner, the armour's over
// the health's, each lit from the left on a dark ground, measured from its screens (640 by 480): from 31 to 204
// across and 13 to 30 down, then 33 to 50, of the view's height, so in any shape of screen. The armour is teal; the
// health green, going through yellow to red as it falls. Read from the view's player: health [+0x1CC] of the most
// [+0x1D8], a player's armour [[+0x7C]+0x78] of [[+0x7C]+0x7C], as 0x13A2C0 reads them
namespace {
constexpr float kTs1Left = 31.0f / 480, kTs1Width = 173.0f / 480, kTs1Top = 13.0f / 480, kTs1Height = 17.0f / 480,
                kTs1Gap = 3.0f / 480;
constexpr uint32_t kTs1Ground = 0x1A1D1E50, kTs1Armour = 0x0C2C346E;

float Part(float now, float most) { return most > 0 && std::isfinite(now) ? std::clamp(now / most, 0.0f, 1.0f) : 0; }

uint32_t Ts1Health(float part) {
    // green 0x0C3D0C at full, yellow at half, red under it
    float r = part > 0.5f ? 12 + (63 - 12) * (1 - part) * 2 : 63;
    float g = part > 0.5f ? 61 : 61 * part * 2;
    return uint32_t(r) << 24 | uint32_t(g) << 16 | 12u << 8 | 0x6E;
}

void DrawTs1Bars(X86Context& ctx) {
    uint32_t view = kernel::R32(kView), player = kernel::R32(0x7B0C7C);
    uint32_t body = player ? kernel::R32(player + 0x14) : 0;
    if (!view || !body) return;
    float health = Part(F(body + 0x1CC), F(body + 0x1D8)), armour = 0;
    if (kernel::R32(body + 0x20) == 8) {
        uint32_t p = kernel::R32(body + 0x7C);
        if (p) armour = Part(F(p + 0x78), F(p + 0x7C));
    }
    float h = float(int32_t(kernel::R32(view + 0x18)) - int32_t(kernel::R32(view + 0x14)));
    float left = float(int32_t(kernel::R32(view))) + h * kTs1Left, right = left + h * kTs1Width;
    float top = float(int32_t(kernel::R32(view + 0x14))) + h * kTs1Top, bh = h * kTs1Height;
    kernel::CallGuest(ctx, kSetTexture, {0xFFFFFFFFu});
    kernel::CallGuest(ctx, kBlendMode, {1});
    for (int i = 0; i < 2; i++) {
        float t = top + float(i) * (bh + h * kTs1Gap), part = i ? health : armour;
        float lit = left + (right - left) * part;
        if (lit > left) Fill(ctx, left, t, lit, t + bh, i ? Ts1Health(part) : kTs1Armour);
        if (right > lit) Fill(ctx, lit, t, right, t + bh, kTs1Ground);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGR("hud: TimeSplitters 1's bars, health %.2f armour %.2f", health, armour);
    }
}

// TimeSplitters 1's crosshair: a green cross, its four arms around a gap, of the view's height
void DrawTs1Cross(X86Context& ctx, float cx, float cy) {
    uint32_t view = kernel::R32(kView);
    float h = view ? float(int32_t(kernel::R32(view + 0x18)) - int32_t(kernel::R32(view + 0x14))) : 480;
    float k = h / 480, arm = 7 * k, gap = 3 * k, half = std::max(1.0f, 1.0f * k);
    constexpr uint32_t kGreen = 0x147F1464;
    kernel::CallGuest(ctx, kSetTexture, {0xFFFFFFFFu});
    kernel::CallGuest(ctx, kBlendMode, {1});
    Fill(ctx, cx - gap - arm, cy - half, cx - gap, cy + half, kGreen);
    Fill(ctx, cx + gap, cy - half, cx + gap + arm, cy + half, kGreen);
    Fill(ctx, cx - half, cy - gap - arm, cx + half, cy - gap, kGreen);
    Fill(ctx, cx - half, cy + gap, cx + half, cy + gap + arm, kGreen);
}
}  // namespace

// a view's HUD: no bars drawn yet; in TimeSplitters 1's style its bars after it
X86_EXTERN_FUNC(__imp__sub_0014CC80);
X86_FUNC(sub_0014CC80) {
    g_bars = 0;
    __imp__sub_0014CC80(ctx, base);
    if (game::Opt().hud_style == 2) DrawTs1Bars(ctx);
}

// a quad of one colour (left, top, right, bottom, u0, v0, u1, v1, colour): the view's crosshair of a sprite from
// 0x14C970, TimeSplitters 1's cross there in its style, on the sprite's centre
X86_EXTERN_FUNC(__imp__sub_0001B7B0);
X86_FUNC(sub_0001B7B0) {
    constexpr uint32_t kFromCrosshair = 0x14CC10, kFromRadar = 0x13427F, kFromPulse = 0x134C9B;
    uint32_t from = kernel::R32(ctx.esp);
    if (from == kFromCrosshair && game::Opt().hud_style == 2) {
        uint32_t a = ctx.esp + 4;
        DrawTs1Cross(ctx, (F(a) + F(a + 8)) * 0.5f, (F(a + 4) + F(a + 12)) * 0.5f);
        return kernel::Return(ctx, 0, 0);
    }
    if ((from == kFromRadar || from == kFromPulse) && game::Opt().hud_style == 1)
        kernel::W32(ctx.esp + 36, Blue(kernel::R32(ctx.esp + 36)));
    __imp__sub_0001B7B0(ctx, base);
}

// DrawBar(left, top, right, bottom, ...): a health bar with its name, the top centre's
X86_EXTERN_FUNC(__imp__sub_00130700);
X86_FUNC(sub_00130700) {
    // (left, top, right, bottom, health, top of a loss or -1, its fade, alpha, 0, name, colour, 0): the health
    // given falls to the new one as a loss fades (0x139AD0)
    uint32_t a = ctx.esp + 4;
    if (game::Opt().hud_style == 1 && DrawTs2Bar(ctx, F(a + 16), kernel::R32(a + 36)))
        return kernel::Return(ctx, 0, 0);
    __imp__sub_00130700(ctx, base);
}

// DrawBar(left, top, right, bottom, ...): the bottom centre's
X86_EXTERN_FUNC(__imp__sub_00130400);
X86_FUNC(sub_00130400) {
    // (left, top, right, bottom, health, -1, fade, 1, 0, name), as the top centre's (0x139740)
    uint32_t a = ctx.esp + 4;
    if (game::Opt().hud_style == 1 && DrawTs2Bar(ctx, F(a + 16), kernel::R32(a + 36)))
        return kernel::Return(ctx, 0, 0);
    __imp__sub_00130400(ctx, base);
}
