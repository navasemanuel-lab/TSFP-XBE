// Pictures of an Xbox Series, PlayStation 4, PlayStation 5 and Switch Pro controller, drawn here rather than shipped as
// images: the body and its parts as smooth shapes with a light from above, each control where the real controller
// has it. They are drawn in the units of the screen's 229 by 144 and stretched to the texture, which the screen squeezes
// back, so circles come out round.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "common/pad_picture.h"

namespace pad_picture {

namespace {

using namespace Gdiplus;
using Style = icons::Style;

constexpr float kMid = kWidth / 2;

bool Start() {
    static bool ok = [] {
        GdiplusStartupInput in;
        ULONG_PTR token = 0;
        return GdiplusStartup(&token, &in, nullptr) == Ok;
    }();
    return ok;
}

Color Rgb(uint32_t rgb, BYTE a = 255) { return Color(a, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)); }

// a colour lighter (above 0) or darker
uint32_t Shade(uint32_t rgb, float by) {
    auto ch = [&](int sh) {
        float c = float((rgb >> sh) & 0xFF);
        c = by > 0 ? c + (255 - c) * by : c * (1 + by);
        return uint32_t(std::clamp(c, 0.0f, 255.0f)) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

// the body's outline from its right half, top middle to bottom middle, mirrored for the left
void Outline(GraphicsPath& path, std::initializer_list<PointF> right) {
    std::vector<PointF> pts(right);
    for (size_t i = right.size() - 2; i >= 1; i--) pts.push_back({2 * kMid - pts[i].X, pts[i].Y});
    path.AddClosedCurve(pts.data(), INT(pts.size()), 0.5f);
}

void Mirror(std::vector<PointF>& pts) {
    for (size_t i = pts.size() - 2; i >= 1; i--) pts.push_back({2 * kMid - pts[i].X, pts[i].Y});
}

void RoundRect(GraphicsPath& path, float l, float t, float r, float b, float rad) {
    float d = std::min({rad * 2, r - l, b - t});
    path.AddArc(l, t, d, d, 180, 90);
    path.AddArc(r - d, t, d, d, 270, 90);
    path.AddArc(r - d, b - d, d, d, 0, 90);
    path.AddArc(l, b - d, d, d, 90, 90);
    path.CloseFigure();
}

void Circle(Graphics& g, const Brush& b, float x, float y, float r) { g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r); }
void Ring(Graphics& g, const Pen& p, float x, float y, float r) { g.DrawEllipse(&p, x - r, y - r, 2 * r, 2 * r); }

// a soft shadow under a shape
void DropShadow(Graphics& g, const GraphicsPath& path, float depth) {
    for (int i = 6; i >= 1; i--) {
        std::unique_ptr<GraphicsPath> p(path.Clone());
        Matrix m;
        m.Translate(0, depth * i / 6);
        p->Transform(&m);
        Pen spread(Color(BYTE(14), 0, 0, 0), float(i) * depth / 4);
        spread.SetLineJoin(LineJoinRound);
        g.DrawPath(&spread, p.get());
        SolidBrush fill(Color(BYTE(16), 0, 0, 0));
        g.FillPath(&fill, p.get());
    }
}

// a shape lit from above: its colour lighter at the top, a sheen across its upper part and a darker rim
void Lit(Graphics& g, const GraphicsPath& path, uint32_t colour, float sheen = 0.18f) {
    RectF box;
    path.GetBounds(&box);
    LinearGradientBrush grad(PointF(0, box.Y - 1), PointF(0, box.GetBottom() + 1), Rgb(Shade(colour, 0.12f)),
                             Rgb(Shade(colour, -0.35f)));
    g.FillPath(&grad, &path);
    GraphicsPath light;
    light.AddEllipse(box.X + box.Width * 0.12f, box.Y - box.Height * 0.25f, box.Width * 0.76f, box.Height * 0.75f);
    PathGradientBrush glow(&light);
    glow.SetCenterColor(Color(BYTE(255 * sheen), 255, 255, 255));
    Color edge(0, 255, 255, 255);
    INT one = 1;
    glow.SetSurroundColors(&edge, &one);
    Region clip(&path);
    g.SetClip(&clip);
    g.FillPath(&glow, &light);
    g.ResetClip();
    Pen rim(Rgb(Shade(colour, -0.55f), 230), 0.7f);
    g.DrawPath(&rim, &path);
}

// a stick: the well it sits in, its cap lit from above and the dished top with its grip ring
void Stick(Graphics& g, float x, float y, float r, uint32_t cap) {
    SolidBrush well(Rgb(Shade(cap, -0.75f)));
    Circle(g, well, x, y + 0.6f, r * 1.22f);
    Pen lip(Rgb(Shade(cap, 0.25f), 70), 0.6f);
    Ring(g, lip, x, y + 0.6f, r * 1.22f);
    GraphicsPath body;
    body.AddEllipse(x - r, y - r, 2 * r, 2 * r);
    DropShadow(g, body, 1.2f);
    Lit(g, body, cap, 0.25f);
    // the dish: dark under its upper edge, catching the light at its lower
    float d = r * 0.74f;
    LinearGradientBrush dish(PointF(0, y - d), PointF(0, y + d), Rgb(Shade(cap, -0.45f)), Rgb(Shade(cap, 0.18f)));
    Circle(g, dish, x, y, d);
    Pen grip(Rgb(Shade(cap, -0.6f), 150), 0.9f);
    Ring(g, grip, x, y, d);
    Pen fine(Rgb(Shade(cap, 0.3f), 40), 0.4f);
    Ring(g, fine, x, y, d * 0.82f);
}

enum class Mark : uint8_t { None, Cross, Circle, Square, Triangle };

// a symbol or letters centred on a point
void Label(Graphics& g, float x, float y, float size, const wchar_t* text, uint32_t colour) {
    FontFamily family(L"Segoe UI");
    Font font(&family, size, FontStyleBold, UnitWorld);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(Rgb(colour));
    g.DrawString(text, -1, &font, RectF(x - size * 2, y - size * 1.5f + size * 0.06f, size * 4, size * 3), &fmt, &b);
}

void Symbol(Graphics& g, float x, float y, float r, Mark mark, uint32_t colour) {
    Pen p(Rgb(colour), r * 0.2f);
    p.SetLineJoin(LineJoinRound);
    float s = r * 0.5f;
    switch (mark) {
        case Mark::Cross:
            g.DrawLine(&p, x - s, y - s, x + s, y + s);
            g.DrawLine(&p, x + s, y - s, x - s, y + s);
            break;
        case Mark::Circle: Ring(g, p, x, y, s); break;
        case Mark::Square: g.DrawRectangle(&p, x - s * 0.9f, y - s * 0.9f, s * 1.8f, s * 1.8f); break;
        case Mark::Triangle: {
            PointF t[3] = {{x, y - s * 1.05f}, {x + s * 1.05f, y + s * 0.75f}, {x - s * 1.05f, y + s * 0.75f}};
            g.DrawPolygon(&p, t, 3);
            break;
        }
        default: break;
    }
}

// a round face button: domed, with its letter or symbol
void Button(Graphics& g, float x, float y, float r, uint32_t cap, const wchar_t* text, Mark mark, uint32_t ink) {
    GraphicsPath body;
    body.AddEllipse(x - r, y - r, 2 * r, 2 * r);
    DropShadow(g, body, 0.9f);
    Lit(g, body, cap, 0.35f);
    if (mark != Mark::None) Symbol(g, x, y, r, mark, ink);
    else Label(g, x, y, r * 1.15f, text, ink);
    // a glint at its top left
    SolidBrush glint(Color(BYTE(70), 255, 255, 255));
    g.FillEllipse(&glint, x - r * 0.55f, y - r * 0.75f, r * 0.6f, r * 0.35f);
}

// four face buttons in a diamond: top, right, bottom, left
struct Face {
    const wchar_t* text;
    Mark mark;
    uint32_t cap, ink;
};
void Diamond(Graphics& g, float x, float y, float gap, float r, const Face (&b)[4]) {
    const float dx[4] = {0, gap, 0, -gap}, dy[4] = {-gap, 0, gap, 0};
    for (int i = 0; i < 4; i++) Button(g, x + dx[i], y + dy[i], r, b[i].cap, b[i].text, b[i].mark, b[i].ink);
}

// a cross shaped pad in its round well (Xbox, Switch)
void CrossPad(Graphics& g, float x, float y, float size, uint32_t colour, bool well) {
    if (well) {
        SolidBrush w(Rgb(Shade(colour, -0.7f)));
        Circle(g, w, x, y + 0.5f, size * 0.78f);
        Pen lip(Rgb(Shade(colour, 0.3f), 60), 0.6f);
        Ring(g, lip, x, y + 0.5f, size * 0.78f);
    }
    float a = size * 0.19f, l = size * 0.62f;
    GraphicsPath h, v;
    RoundRect(h, x - l, y - a, x + l, y + a, a * 0.5f);
    RoundRect(v, x - a, y - l, x + a, y + l, a * 0.5f);
    GraphicsPath cross(FillModeWinding);
    cross.AddPath(&h, FALSE);
    cross.AddPath(&v, FALSE);
    DropShadow(g, cross, 1.0f);
    LinearGradientBrush grad(PointF(0, y - l), PointF(0, y + l), Rgb(Shade(colour, 0.15f)), Rgb(Shade(colour, -0.3f)));
    g.FillPath(&grad, &cross);
    // both bars' rims, then their insides again over where the rims cross
    Pen rim(Rgb(Shade(colour, -0.55f), 220), 0.6f);
    g.DrawPath(&rim, &h);
    g.DrawPath(&rim, &v);
    g.FillRectangle(&grad, x - a + 0.3f, y - l + 0.3f, 2 * a - 0.6f, 2 * l - 0.6f);
    g.FillRectangle(&grad, x - l + 0.3f, y - a + 0.3f, 2 * l - 0.6f, 2 * a - 0.6f);
    SolidBrush dimple(Rgb(Shade(colour, -0.25f)));
    Circle(g, dimple, x, y, a * 0.45f);
    // the arrows
    SolidBrush arrow(Rgb(Shade(colour, 0.45f), 120));
    float t = a * 0.45f, e = l - a * 0.55f;
    for (int i = 0; i < 4; i++) {
        float ux = i == 1 ? 1.0f : i == 3 ? -1.0f : 0.0f, uy = i == 0 ? -1.0f : i == 2 ? 1.0f : 0.0f;
        PointF p[3] = {{x + ux * e, y + uy * e},
                       {x + ux * (e - t * 1.4f) - uy * t, y + uy * (e - t * 1.4f) + ux * t},
                       {x + ux * (e - t * 1.4f) + uy * t, y + uy * (e - t * 1.4f) - ux * t}};
        g.FillPolygon(&arrow, p, 3);
    }
}

// a PlayStation pad: four separate arrow keys pointing in
void ArrowPad(Graphics& g, float x, float y, float size, uint32_t colour, uint32_t ink) {
    float w = size * 0.44f, inner = size * 0.2f, outer = size;
    for (int i = 0; i < 4; i++) {
        float ux = i == 1 ? 1.0f : i == 3 ? -1.0f : 0.0f, uy = i == 0 ? -1.0f : i == 2 ? 1.0f : 0.0f;
        auto at = [&](float along, float across) { return PointF(x + ux * along - uy * across, y + uy * along + ux * across); };
        PointF p[5] = {at(inner, 0), at(inner + w * 0.5f, -w * 0.5f), at(outer, -w * 0.5f), at(outer, w * 0.5f),
                       at(inner + w * 0.5f, w * 0.5f)};
        GraphicsPath key;
        key.AddPolygon(p, 5);
        DropShadow(g, key, 0.9f);
        Lit(g, key, colour, 0.3f);
        // its arrow
        float m = (inner + outer) * 0.5f + w * 0.15f, s = w * 0.2f;
        PointF a[3] = {at(m + s, 0), at(m - s * 0.6f, -s), at(m - s * 0.6f, s)};
        SolidBrush b(Rgb(ink));
        g.FillPolygon(&b, a, 3);
    }
}

// a small button: a pill or a round one
void Small(Graphics& g, float x, float y, float w, float h, uint32_t colour) {
    GraphicsPath p;
    if (w == h) p.AddEllipse(x - w / 2, y - h / 2, w, h);
    else RoundRect(p, x - w / 2, y - h / 2, x + w / 2, y + h / 2, std::min(w, h) / 2);
    DropShadow(g, p, 0.6f);
    Lit(g, p, colour, 0.3f);
}

// the shoulder buttons peeping over the body's top
void Shoulders(Graphics& g, float l, float r, float top, float bottom, uint32_t colour) {
    for (int side = 0; side < 2; side++) {
        float a = side ? 2 * kMid - r : l, b = side ? 2 * kMid - l : r;
        GraphicsPath p;
        RoundRect(p, a, top, b, bottom, 6);
        Lit(g, p, colour, 0.25f);
    }
}

// the body: shadow, lit fill
void Body(Graphics& g, const GraphicsPath& body, uint32_t colour, float sheen) {
    DropShadow(g, body, 3.5f);
    Lit(g, body, colour, sheen);
}

// --- the controllers, in the screen's units: 229 across, 144 down

void Outline(Style style, GraphicsPath& body) {
    switch (style) {
        case Style::Xbox:
            Outline(body, {{kMid, 17}, {140, 14}, {170, 9}, {196, 13}, {212, 30}, {219, 58}, {223, 92}, {224, 120},
                           {214, 138}, {197, 140}, {182, 126}, {168, 108}, {146, 100}, {kMid, 99}});
            break;
        case Style::Switch:
            Outline(body, {{kMid, 16}, {142, 13}, {172, 9}, {198, 13}, {213, 30}, {219, 58}, {222, 94}, {222, 124},
                           {212, 140}, {195, 141}, {181, 126}, {168, 106}, {146, 99}, {kMid, 98}});
            break;
        case Style::PlayStation5:
            Outline(body, {{kMid, 13}, {150, 13}, {176, 9}, {200, 12}, {214, 26}, {221, 52}, {225, 90}, {226, 122},
                           {218, 140}, {201, 142}, {186, 128}, {172, 110}, {152, 108}, {kMid, 114}});
            break;
        default:  // PlayStation 4
            Outline(body, {{kMid, 14}, {150, 14}, {174, 10}, {198, 13}, {212, 27}, {219, 52}, {223, 88}, {224, 118},
                           {216, 136}, {200, 139}, {186, 126}, {172, 108}, {152, 104}, {kMid, 108}});
            break;
    }
}

void Xbox(Graphics& g) {
    constexpr uint32_t kBody = 0x2B2C30, kPart = 0x26272B, kInk = 0x18191C;
    Shoulders(g, 28, 92, 5, 18, 0x2E2F33);
    GraphicsPath body;
    Outline(Style::Xbox, body);
    Body(g, body, kBody, 0.16f);
    // the guide button, lit round its edge
    GraphicsPath guide;
    guide.AddEllipse(kMid - 8.5f, 31 - 8.5f, 17.0f, 17.0f);
    Lit(g, guide, 0x34353A, 0.3f);
    Pen ring(Color(BYTE(235), 235, 238, 242), 1.3f);
    Ring(g, ring, kMid, 31, 6.2f);
    Pen halo(Color(BYTE(60), 255, 255, 255), 3.0f);
    Ring(g, halo, kMid, 31, 6.2f);
    // view, share, menu
    Small(g, 100, 50, 6.4f, 6.4f, kPart);
    Small(g, 129, 50, 6.4f, 6.4f, kPart);
    Small(g, kMid, 59, 7, 3.6f, kPart);
    SolidBrush mark(Color(BYTE(170), 200, 202, 208));
    g.FillRectangle(&mark, 98.6f, 48.6f, 2.0f, 1.6f);
    g.FillRectangle(&mark, 100.0f, 49.8f, 2.0f, 1.6f);
    for (int i = 0; i < 3; i++) g.FillRectangle(&mark, 127.3f, 48.4f + i * 1.3f, 3.4f, 0.6f);
    Stick(g, 63, 50, 15, 0x2A2B2F);
    CrossPad(g, 90, 88, 18, 0x303135, true);
    Stick(g, 142, 88, 15, 0x2A2B2F);
    Diamond(g, 169, 50, 13, 6.8f,
            {{L"Y", Mark::None, kInk, 0xF5C518}, {L"B", Mark::None, kInk, 0xE5383B},
             {L"A", Mark::None, kInk, 0x6CC24A}, {L"X", Mark::None, kInk, 0x2D8CFF}});
}

void Switch(Graphics& g) {
    constexpr uint32_t kBody = 0x2A2B2F, kCap = 0x1A1B1E, kInk = 0xD8D8DC;
    Shoulders(g, 30, 92, 5, 18, 0x303136);
    GraphicsPath body;
    Outline(Style::Switch, body);
    Body(g, body, kBody, 0.2f);
    // minus and plus
    SolidBrush ink(Rgb(0xC8C8CC));
    g.FillRectangle(&ink, 89.5f, 30.0f, 7.0f, 2.0f);
    g.FillRectangle(&ink, 133.0f, 30.0f, 7.0f, 2.0f);
    g.FillRectangle(&ink, 135.5f, 27.5f, 2.0f, 7.0f);
    // capture: a square with a dot, home: round with a ring
    GraphicsPath capture;
    RoundRect(capture, 96.5f, 41.5f, 103.5f, 48.5f, 1.2f);
    Lit(g, capture, 0x26272B, 0.3f);
    SolidBrush dot(Rgb(0x55565C));
    Circle(g, dot, 100, 45, 1.8f);
    GraphicsPath home;
    home.AddEllipse(129 - 4.2f, 45 - 4.2f, 8.4f, 8.4f);
    Lit(g, home, 0x26272B, 0.3f);
    Pen silver(Rgb(0xA8AAB0), 0.8f);
    Ring(g, silver, 129, 45, 4.6f);
    Stick(g, 64, 52, 14.5f, 0x26272B);
    CrossPad(g, 91, 88, 17, 0x2C2D31, true);
    Stick(g, 140, 88, 14.5f, 0x26272B);
    Diamond(g, 167, 50, 13, 6.5f,
            {{L"X", Mark::None, kCap, kInk}, {L"A", Mark::None, kCap, kInk},
             {L"B", Mark::None, kCap, kInk}, {L"Y", Mark::None, kCap, kInk}});
}

void PlayStation(Graphics& g, bool ps5) {
    uint32_t body_colour = ps5 ? 0xECEDF1 : 0x30333A;
    Shoulders(g, 30, 90, 5, 17, ps5 ? 0xDADBE0 : 0x2C2F35);
    GraphicsPath body;
    Outline(ps5 ? Style::PlayStation5 : Style::PlayStation4, body);
    Body(g, body, body_colour, ps5 ? 0.25f : 0.16f);
    if (ps5) {
        // the black middle the sticks sit in
        std::vector<PointF> pts = {{kMid, 60}, {147, 60}, {165, 74}, {172, 100}, {160, 113}, {138, 116}, {kMid, 121}};
        Mirror(pts);
        GraphicsPath plate;
        plate.AddClosedCurve(pts.data(), INT(pts.size()), 0.5f);
        Region clip(&body);
        g.SetClip(&clip);
        Lit(g, plate, 0x1E1F23, 0.12f);
        g.ResetClip();
    }
    // the touch pad, with the PS5's light bar down its sides
    GraphicsPath pad;
    RoundRect(pad, 80, 15, 149, 56, 7);
    Lit(g, pad, ps5 ? 0xE4E5E9 : 0x2A2C32, ps5 ? 0.3f : 0.22f);
    if (ps5) {
        for (float x : {77.5f, 2 * kMid - 77.5f}) {
            Pen glow(Color(BYTE(70), 70, 150, 255), 4.5f), bar(Color(BYTE(235), 90, 165, 255), 1.6f);
            glow.SetStartCap(LineCapRound);
            glow.SetEndCap(LineCapRound);
            bar.SetStartCap(LineCapRound);
            bar.SetEndCap(LineCapRound);
            g.DrawLine(&glow, x, 22.0f, x, 52.0f);
            g.DrawLine(&bar, x, 22.0f, x, 52.0f);
        }
    }
    // create (share) and options
    uint32_t buttons = ps5 ? 0x2A2B30 : 0x23252A;
    Small(g, 73, 22, 3.2f, 7.5f, buttons);
    Small(g, 2 * kMid - 73, 22, 3.2f, 7.5f, buttons);
    // the PS button, and the PS5's mute button under it
    float sticks_y = ps5 ? 95.0f : 92.0f;
    GraphicsPath ps;
    ps.AddEllipse(kMid - 4.4f, sticks_y + 3 - 4.4f, 8.8f, 8.8f);
    Lit(g, ps, 0x1B1C20, 0.3f);
    if (ps5) Small(g, kMid, sticks_y + 12, 9, 2.8f, 0x34353A);
    ArrowPad(g, 50, 48, 18, ps5 ? 0xE2E3E7 : 0x3A3D44, ps5 ? 0x9A9DA6 : 0x8A8E98);
    Stick(g, 86, sticks_y, 14, 0x232428);
    Stick(g, 143, sticks_y, 14, 0x232428);
    uint32_t cap = ps5 ? 0xD8D9DE : 0x25272C, plain = 0x8C9099;
    Diamond(g, 179, 48, 13, 6.6f,
            {{L"", Mark::Triangle, cap, ps5 ? plain : 0x40C9A2}, {L"", Mark::Circle, cap, ps5 ? plain : 0xF0626B},
             {L"", Mark::Cross, cap, ps5 ? plain : 0x7EB0E8}, {L"", Mark::Square, cap, ps5 ? plain : 0xE59AD0}});
}

// the drawing, stretched to w by h; units are the screen's (or the halo's) when not
std::vector<uint32_t> Draw(uint32_t w, uint32_t h, float units_w, float units_h, float dx, float dy,
                           void (*draw)(Graphics&, Style), Style style) {
    std::vector<uint32_t> px(size_t(w) * h, 0);
    if (!Start()) return px;
    Bitmap bmp(INT(w), INT(h), PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        g.ScaleTransform(float(w) / units_w, float(h) / units_h);
        g.TranslateTransform(dx, dy);
        draw(g, style);
    }
    BitmapData data;
    Rect all(0, 0, INT(w), INT(h));
    if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
        for (uint32_t y = 0; y < h; y++)
            std::memcpy(&px[size_t(y) * w], static_cast<uint8_t*>(data.Scan0) + ptrdiff_t(y) * data.Stride, w * 4);
        bmp.UnlockBits(&data);
    }
    return px;
}

bool Drawn(Style style) {
    return style == Style::Xbox || style == Style::PlayStation4 || style == Style::PlayStation5 || style == Style::Switch;
}

std::mutex g_lock;  // GDI+ objects are drawn one at a time

}  // namespace

std::vector<uint32_t> Picture(Style style, uint32_t w, uint32_t h) {
    if (!Drawn(style)) return {};
    std::lock_guard lock(g_lock);
    return Draw(w, h, kWidth, kHeight, 0, 0, [](Graphics& g, Style s) {
        switch (s) {
            case Style::Xbox: Xbox(g); break;
            case Style::Switch: Switch(g); break;
            default: PlayStation(g, s == Style::PlayStation5); break;
        }
    }, style);
}

std::vector<uint32_t> Glow(Style style, uint32_t w, uint32_t h) {
    if (!Drawn(style)) return {};
    std::lock_guard lock(g_lock);
    // the body grown by a few units, its edge softened, in the game's grey
    std::vector<uint32_t> px = Draw(w, h, kGlowWidth, kGlowHeight, (kGlowWidth - kWidth) / 2, (kGlowHeight - kHeight) / 2,
                                    [](Graphics& g, Style s) {
                                        GraphicsPath body;
                                        Outline(s, body);
                                        SolidBrush fill(Color(255, 255, 255, 255));
                                        g.FillPath(&fill, &body);
                                        Pen grow(Color(255, 255, 255, 255), 12);
                                        grow.SetLineJoin(LineJoinRound);
                                        g.DrawPath(&grow, &body);
                                    }, style);
    std::vector<uint32_t> soft(px.size());
    int r = int(std::max(w, h) / 64) + 1;
    for (int pass = 0; pass < 2; pass++) {
        // a box blur of the alpha, across then down
        for (int horizontal = 1; horizontal >= 0; horizontal--) {
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t sum = 0, n = 0;
                    for (int k = -r; k <= r; k++) {
                        int sx = int(x) + (horizontal ? k : 0), sy = int(y) + (horizontal ? 0 : k);
                        if (sx < 0 || sy < 0 || sx >= int(w) || sy >= int(h)) {
                            n++;
                            continue;
                        }
                        sum += px[size_t(sy) * w + sx] >> 24;
                        n++;
                    }
                    soft[size_t(y) * w + x] = (sum / n) << 24;
                }
            px.swap(soft);
        }
    }
    for (uint32_t& p : px) p = (p & 0xFF000000u) | 0x868686;
    return px;
}

}  // namespace pad_picture
