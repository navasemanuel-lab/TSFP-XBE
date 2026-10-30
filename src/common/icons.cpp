// The game's controller button icons swapped for those of the device in use: keyboard keys, or the buttons of an Xbox
// Series, PlayStation 4, PlayStation 5 or Switch controller. A button keeps its place: the game's A is the bottom button,
// which a PlayStation pad marks with a cross and a Switch pad names B.
// The controllers' buttons are the pictures in prompts\<xbox|ps4|ps5|switch>\ beside the exe (a, b, x, y, start, white,
// black: the game's buttons, the Switch's by place), else drawn here.
// TimeSplitters FP draws a prompt's button from a text code (0x0B then '1' to '<'): the code's index picks a texture
// id from a table, and the texture objects (0x34 bytes, the D3D texture header first) sit in an array. A swap points
// the icon's header at one drawn here; going back restores the game's own data and format.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "common/controls.h"
#include "common/icons.h"
#include "app/app.h"
#include "core/log.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace icons {

namespace {

constexpr uint32_t kTextureObjects = 0x4B85C8;  // points at the pointer to the texture object array
constexpr uint32_t kObjectSize = 0x34;
constexpr uint32_t kButtonIds = 0x7E72A0;       // texture ids by prompt code index
constexpr uint32_t kFormatA8R8G8B8 = 0x06;      // swizzled

struct Icon {
    controls::Button button;
    uint32_t w, h;          // drawn at twice the game's icon for sharpness
    bool rotated;           // the game's START icon is stored upside down
    uint64_t fingerprint;   // of the game's icon: loads reuse texture ids before the prompt table follows
};
// prompt code index 0 to 6: B, A, Y, X, START, white, black
constexpr Icon kIcons[] = {
    {controls::Button::B, 64, 64, false, 0x310308B60701E658},     {controls::Button::A, 64, 64, false, 0x9C0684AF8FDB8F8E},
    {controls::Button::Y, 64, 64, false, 0x6E142E1F065A2E98},     {controls::Button::X, 64, 64, false, 0x4FA5C3134B963D19},
    {controls::Button::Start, 128, 64, true, 0x10A84C7687451767}, {controls::Button::White, 64, 64, false, 0x98DBB08E23DD85C0},
    {controls::Button::Black, 64, 64, false, 0x115AEE4B0A8E851D},
};
constexpr size_t kCount = std::size(kIcons);
constexpr size_t kStyles = 6;

// one style's icons as console textures: physical address and format
struct Set {
    bool made = false;
    uint32_t data[kCount] = {}, format[kCount] = {};
    uint32_t va[kCount] = {};  // the pixels, drawn over again in place
};
bool g_keys_changed = false;
Set g_sets[kStyles];
uint32_t g_game_data[kCount] = {}, g_game_format[kCount] = {};  // the icon's own, while another is in place
std::mutex g_lock;
Style g_style = Style::Original;

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) n++;
    return n;
}

// the console's texel order for a power of two texture: x and y bits interleaved while both dimensions last
uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t off = 0, bit = 0;
    for (uint32_t sx = 1, sy = 1; sx < w || sy < h;) {
        if (sx < w) {
            off |= (x & sx ? 1u : 0u) << bit++;
            sx <<= 1;
        }
        if (sy < h) {
            off |= (y & sy ? 1u : 0u) << bit++;
            sy <<= 1;
        }
    }
    return off;
}

// a GDI drawing surface, 32 bit top down
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* px = nullptr;
    int w, h;
    Canvas(int w_, int h_) : w(w_), h(h_) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 32, BI_RGB};
        void* bits = nullptr;
        dc = CreateCompatibleDC(nullptr);
        if (dc) bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) old = SelectObject(dc, bmp);
        px = static_cast<uint32_t*>(bits);
        if (px) std::memset(px, 0, size_t(w) * h * 4);
    }
    ~Canvas() {
        if (old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

// the label in white on black at this size, and the box its pixels cover
RECT DrawLabel(Canvas& c, const std::wstring& text, const wchar_t* face, int size) {
    std::memset(c.px, 0, size_t(c.w) * c.h * 4);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    HGDIOBJ old = SelectObject(c.dc, font);
    SetBkMode(c.dc, TRANSPARENT);
    SetTextColor(c.dc, RGB(255, 255, 255));
    RECT all{0, 0, c.w, c.h};
    DrawTextW(c.dc, text.c_str(), int(text.size()), &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    SelectObject(c.dc, old);
    DeleteObject(font);
    RECT r{c.w, c.h, 0, 0};
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++)
            if (c.px[y * c.w + x] & 0xFF) {
                r.left = std::min<LONG>(r.left, x);
                r.top = std::min<LONG>(r.top, y);
                r.right = std::max<LONG>(r.right, x + 1);
                r.bottom = std::max<LONG>(r.bottom, y + 1);
            }
    return r;
}

// how a button is drawn
enum class Shape : uint8_t { Key, Round, Pill };
enum class Symbol : uint8_t { None, Cross, Circle, Square, Triangle };
struct Look {
    const wchar_t* text;
    uint32_t colour;  // 0xRRGGBB of the text or symbol
    Symbol symbol;
    Shape shape;
};

constexpr uint32_t kWhite = 0xF0F0F0;

Look LookFor(Style style, controls::Button b, std::wstring& keep) {
    using B = controls::Button;
    switch (style) {
        case Style::Xbox:
            switch (b) {
                case B::A: return {L"A", 0x6CC24A, Symbol::None, Shape::Round};
                case B::B: return {L"B", 0xE5383B, Symbol::None, Shape::Round};
                case B::X: return {L"X", 0x2D8CFF, Symbol::None, Shape::Round};
                case B::Y: return {L"Y", 0xF5C518, Symbol::None, Shape::Round};
                case B::Start: return {L"≡", kWhite, Symbol::None, Shape::Pill};  // the menu button's lines
                case B::White: return {L"LB", kWhite, Symbol::None, Shape::Pill};
                default: return {L"RB", kWhite, Symbol::None, Shape::Pill};
            }
        case Style::PlayStation4:
        case Style::PlayStation5: {
            bool ps5 = style == Style::PlayStation5;  // the PS5's symbols are plain, the PS4's coloured
            switch (b) {
                case B::A: return {L"", ps5 ? kWhite : 0x7EB0E8, Symbol::Cross, Shape::Round};
                case B::B: return {L"", ps5 ? kWhite : 0xF0626B, Symbol::Circle, Shape::Round};
                case B::X: return {L"", ps5 ? kWhite : 0xE59AD0, Symbol::Square, Shape::Round};
                case B::Y: return {L"", ps5 ? kWhite : 0x40C9A2, Symbol::Triangle, Shape::Round};
                case B::Start: return {L"OPTIONS", kWhite, Symbol::None, Shape::Pill};
                case B::White: return {L"L1", kWhite, Symbol::None, Shape::Pill};
                default: return {L"R1", kWhite, Symbol::None, Shape::Pill};
            }
        }
        case Style::Switch:
            // by place: the bottom button is B, the right A, the left Y, the top X
            switch (b) {
                case B::A: return {L"B", kWhite, Symbol::None, Shape::Round};
                case B::B: return {L"A", kWhite, Symbol::None, Shape::Round};
                case B::X: return {L"Y", kWhite, Symbol::None, Shape::Round};
                case B::Y: return {L"X", kWhite, Symbol::None, Shape::Round};
                case B::Start: return {L"+", kWhite, Symbol::None, Shape::Round};
                case B::White: return {L"L", kWhite, Symbol::None, Shape::Pill};
                default: return {L"R", kWhite, Symbol::None, Shape::Pill};
            }
        default: {
            // keys with long names show their symbol, which reads at the icon's size
            const char* label = controls::KeyLabel(b);
            keep.assign(label, label + std::strlen(label));
            if (keep == L"ENTER") keep = L"↵";
            if (keep == L"BKSP") keep = L"⌫";
            return {keep.c_str(), kWhite, Symbol::None, Shape::Key};
        }
    }
}

COLORREF Gdi(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// the button's background: a key cap, a round face button or a pill for the shoulders and menus
void DrawShape(Canvas& c, Shape shape) {
    int w = c.w, h = c.h, m = std::max(2, h / 32);
    HBRUSH fill = CreateSolidBrush(RGB(40, 40, 46));
    HPEN edge = CreatePen(PS_SOLID, std::max(2, h / 20), RGB(225, 225, 225));
    HGDIOBJ ob = SelectObject(c.dc, fill), op = SelectObject(c.dc, edge);
    if (shape == Shape::Round) {
        int d = std::min(w, h) - 2 * m;
        Ellipse(c.dc, (w - d) / 2, (h - d) / 2, (w + d) / 2, (h + d) / 2);
    } else {
        int r = shape == Shape::Pill ? h - 2 * m : h / 3;
        int top = shape == Shape::Pill ? h / 6 : m, bottom = shape == Shape::Pill ? h - h / 6 : h - m;
        RoundRect(c.dc, m, top, w - m, bottom, r, r);
    }
    GdiFlush();
    SelectObject(c.dc, ob);
    SelectObject(c.dc, op);
    DeleteObject(fill);
    DeleteObject(edge);
}

// a PlayStation symbol in its colour, centred
void DrawSymbol(Canvas& c, Symbol s, uint32_t colour) {
    int cx = c.w / 2, cy = c.h / 2, r = c.h * 22 / 100;
    HPEN pen = CreatePen(PS_SOLID, std::max(3, c.h / 11), Gdi(colour));
    HGDIOBJ op = SelectObject(c.dc, pen), ob = SelectObject(c.dc, GetStockObject(NULL_BRUSH));
    switch (s) {
        case Symbol::Cross:
            MoveToEx(c.dc, cx - r, cy - r, nullptr);
            LineTo(c.dc, cx + r, cy + r);
            MoveToEx(c.dc, cx + r, cy - r, nullptr);
            LineTo(c.dc, cx - r, cy + r);
            break;
        case Symbol::Circle: Ellipse(c.dc, cx - r, cy - r, cx + r, cy + r); break;
        case Symbol::Square: Rectangle(c.dc, cx - r, cy - r, cx + r, cy + r); break;
        case Symbol::Triangle: {
            POINT p[3] = {{cx, cy - r - r / 6}, {cx + r + r / 8, cy + r - r / 6}, {cx - r - r / 8, cy + r - r / 6}};
            Polygon(c.dc, p, 3);
            break;
        }
        default: break;
    }
    GdiFlush();
    SelectObject(c.dc, op);
    SelectObject(c.dc, ob);
    DeleteObject(pen);
}

// the button's picture in prompts\ as ARGB rows top down, fitted inside w x h and centred; empty without one
std::vector<uint32_t> LoadButton(Style style, controls::Button button, uint32_t w, uint32_t h) {
    using Microsoft::WRL::ComPtr;
    static const wchar_t* kFolders[] = {nullptr, nullptr, L"xbox", L"ps4", L"ps5", L"switch"};
    static const wchar_t* kNames[] = {L"a", L"b", L"x", L"y", L"black", L"white", L"start", L"back"};
    const wchar_t* folder = kFolders[size_t(style)];
    if (!folder) return {};
    std::wstring path = (app::ExeDir() / L"prompts" / folder / (std::wstring(kNames[int(button)]) + L".png")).wstring();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // already in either mode is as good
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    UINT iw = 0, ih = 0;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&iw, &ih);
    if (FAILED(hr) || !iw || !ih) {
        LOGR("icons: %ls can't be read (0x%08X)", path.c_str(), unsigned(hr));
        return {};
    }
    // as large as fits, its shape kept
    double fit = std::min(double(w) / iw, double(h) / ih);
    UINT sw = std::max<UINT>(1, UINT(iw * fit + 0.5)), sh = std::max<UINT>(1, UINT(ih * fit + 0.5));
    hr = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    std::vector<uint32_t> fitted(size_t(sw) * sh);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, sw * 4, UINT(fitted.size() * 4), reinterpret_cast<BYTE*>(fitted.data()));
    if (FAILED(hr)) {
        LOGR("icons: %ls can't be read (0x%08X)", path.c_str(), unsigned(hr));
        return {};
    }
    std::vector<uint32_t> px(size_t(w) * h, 0);
    uint32_t ox = (w - sw) / 2, oy = (h - sh) / 2;
    for (uint32_t y = 0; y < sh; y++)
        std::memcpy(&px[size_t(oy + y) * w + ox], &fitted[size_t(y) * sw], sw * 4);
    return px;
}

// the button as ARGB rows top down; black is left transparent
std::vector<uint32_t> DrawButton(Style style, controls::Button button, uint32_t w, uint32_t h) {
    if (std::vector<uint32_t> picture = LoadButton(style, button, w, h); !picture.empty()) return picture;
    std::vector<uint32_t> px(size_t(w) * h, 0);
    Canvas cap{int(w), int(h)}, text{int(w) * 2, int(h) * 2};
    if (!cap.px || !text.px) return px;
    std::wstring keep;
    Look look = LookFor(style, button, keep);
    DrawShape(cap, look.shape);
    if (look.symbol != Symbol::None) DrawSymbol(cap, look.symbol, look.colour);
    RECT ink{};
    bool has_text = look.text && *look.text;
    if (has_text) {
        std::wstring s = look.text;
        const wchar_t* face = s.size() == 1 && s[0] > 0x2000 ? L"Segoe UI Symbol" : L"Arial";
        // the label's own pixels as large as fit inside the edge, centred on them
        int room_w = int(w) - int(h) / 3, room_h = int(h) - int(h) / 3;
        if (look.shape == Shape::Round) room_w = room_h = int(h) * 55 / 100;
        for (int size = int(h) * 2; size >= 6; size -= 2) {
            ink = DrawLabel(text, s, face, size);
            if (ink.right - ink.left <= room_w && ink.bottom - ink.top <= room_h) break;
        }
    }
    int dx = (int(w) - (ink.right - ink.left)) / 2 - ink.left, dy = (int(h) - (ink.bottom - ink.top)) / 2 - ink.top;
    uint32_t tc = look.colour;
    for (int y = 0; y < int(h); y++)
        for (int x = 0; x < int(w); x++) {
            uint32_t c = cap.px[y * int(w) + x] & 0xFFFFFF;
            int tx = x - dx, ty = y - dy;
            uint32_t a = has_text && tx >= 0 && ty >= 0 && tx < text.w && ty < text.h ? text.px[ty * text.w + tx] & 0xFF : 0;
            if (c && a) {
                // the text's colour over the cap by the glyph's coverage
                auto mix = [&](int sh) { return (((c >> sh) & 0xFF) * (255 - a) + ((tc >> sh) & 0xFF) * a) / 255 << sh; };
                c = mix(16) | mix(8) | mix(0);
            }
            px[size_t(y) * w + x] = c ? 0xFF000000u | c : 0;
        }
    return px;
}

// a style's icons as console textures in guest memory, once
void MakeSet(Style style) {
    Set& set = g_sets[size_t(style)];
    set.made = true;
    for (size_t i = 0; i < kCount; i++) {
        const Icon& ic = kIcons[i];
        std::vector<uint32_t> px = DrawButton(style, ic.button, ic.w, ic.h);
        uint32_t va = set.va[i] ? set.va[i] : mem::AllocContiguous(ic.w * ic.h * 4, 0, 0xFFFFFFFF, 0x1000);
        if (!va) continue;
        set.va[i] = va;
        auto* out = static_cast<uint32_t*>(kernel::HostPtr(va));
        for (uint32_t y = 0; y < ic.h; y++)
            for (uint32_t x = 0; x < ic.w; x++) {
                uint32_t sx = ic.rotated ? ic.w - 1 - x : x, sy = ic.rotated ? ic.h - 1 - y : y;
                out[Swizzle(x, y, ic.w, ic.h)] = px[size_t(sy) * ic.w + sx];
            }
        set.data[i] = mem::PhysicalFromVirtual(va);
        gpu::MarkDataDirty(va);  // drawn over: the renderer's copy is stale
        // DMA, dimensions and border bits as the console's 2D textures have them, one level
        set.format[i] = 0x29 | kFormatA8R8G8B8 << 8 | 1u << 16 | Log2(ic.w) << 20 | Log2(ic.h) << 24;
    }
}

// the first bytes of a texture's pixels: tells the game's icons from other textures given the same id
uint64_t Fingerprint(uint32_t data) {
    const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(mem::VirtualFromPhysical(data)));
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 256; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// the icon's texture header, 0 before the game has loaded it
uint32_t Header(size_t i) {
    uint32_t objects = kernel::R32(kTextureObjects);
    if (!objects || !(objects = kernel::R32(objects))) return 0;
    uint32_t id = kernel::R32(kButtonIds + uint32_t(i) * 4);
    return id ? objects + id * kObjectSize : 0;
}

// one of the drawn icons, any style
bool Ours(size_t i, uint32_t data) {
    for (const Set& s : g_sets)
        if (s.made && s.data[i] == data) return true;
    return false;
}

}  // namespace

const char* Name(Style s) {
    static const char* kNames[] = {"controller (the game's)", "keyboard", "Xbox", "PlayStation 4", "PlayStation 5", "Switch"};
    return kNames[size_t(s)];
}

void KeysChanged() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_keys_changed = true;
}

Style Current() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_style;
}

void Update(Style style) {
    std::lock_guard<std::mutex> lock(g_lock);
    bool original = style == Style::Original;
    Set& set = g_sets[size_t(style)];
    if (g_keys_changed && g_sets[size_t(Style::Keyboard)].made) MakeSet(Style::Keyboard);
    g_keys_changed = false;
    if (!original && !set.made) MakeSet(style);
    for (size_t i = 0; i < kCount; i++) {
        uint32_t hdr = Header(i);
        if (!hdr) continue;
        uint32_t data = kernel::R32(hdr + 4);
        bool ours = Ours(i, data);
        if (original) {
            if (ours) {
                kernel::W32(hdr + 4, g_game_data[i]);
                kernel::W32(hdr + 12, g_game_format[i]);
            }
            continue;
        }
        if (!set.data[i] || data == set.data[i]) continue;
        if (!ours) {
            if (Fingerprint(data) != kIcons[i].fingerprint) continue;  // not the icon (yet): another texture has the id
            // the game's icon, or a reload of it: kept to put back
            g_game_data[i] = data;
            g_game_format[i] = kernel::R32(hdr + 12);
        }
        kernel::W32(hdr + 4, set.data[i]);
        kernel::W32(hdr + 12, set.format[i]);
    }
    if (g_style != style) LOGI("icons: %s prompts", Name(style));
    g_style = style;
}

}  // namespace icons
