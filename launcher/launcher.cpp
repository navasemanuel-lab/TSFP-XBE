// TSFP Launcher: brings the player's own copy of the game into assets (its disc image, or default.xbe with its
// folders), checks it is the release TSFP.exe was recompiled from, starts the game and keeps it up to date: the
// release's update.txt names a GitHub repository (and, for a private one, a read-only token on its second line) whose
// latest release carries TSFP.zip, its tag the version, which version.txt beside the launcher holds for the installed
// one. Drawn with Direct2D: the banner is the game's cover when the art folder beside the launcher has it
// (art/cover.jpg, and art/logo.png for a cover wider than tall: the release's, not part of this source), then the
// pictures of art/slides in their names' order, one fading into the next; else a time portal made here.
//   --snapshot <file.png> [missing|ready|importing|settings|mods|saves|levels] [seconds]   draws one frame into a picture and quits
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite_1.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe TSFP.exe was recompiled from, written by CMake

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kGameExe[] = L"TSFP.exe";
constexpr wchar_t kXbe[] = L"default.xbe";
constexpr wchar_t kVersion[] = L"LAUNCHER 1.1";
constexpr float kWidth = 1060, kHeight = 700, kBar = 36, kBanner = 440;
constexpr float kPi = 3.14159265f;

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
}
const D2D1_COLOR_F kBack = Rgb(0x0E1117), kBarBack = Rgb(0x090B10), kLine = Rgb(0x262C38), kText = Rgb(0xE9EBF0),
                   kDim = Rgb(0x8B91A0), kFaint = Rgb(0x5A6070), kCyan = Rgb(0x52C8FF), kOrange = Rgb(0xE0561C),
                   kBox = Rgb(0x141821), kButton = Rgb(0x1A1F29), kRed = Rgb(0xFF6A5A);

// ---- the game in assets ----------------------------------------------------------------------------------------

fs::path ExeDir() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    return fs::path(std::wstring(path, n)).parent_path();
}

// assets beside the exe or in a folder above it, as the game looks; else made beside the exe
fs::path Assets() {
    std::error_code ec;
    fs::path dir = ExeDir();
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return ExeDir() / "assets";
}

std::string Sha1(const std::vector<uint8_t>& data) {
    uint8_t digest[20] = {};
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, PUCHAR(data.data()), ULONG(data.size()), digest, 20);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ::ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

enum class Found { Checking, Missing, Wrong, Broken, Ready };

struct Check {
    Found found = Found::Missing;
    std::wstring name;    // the copy found
    std::wstring detail;  // what is wrong with it
};

// every file of the image inside it: a cut-short copy has some past its end
bool Whole(const kernel::Disc::Entry& dir, uint64_t size) {
    for (const auto& e : dir.children)
        if (e.dir ? !Whole(e, size) : e.offset + e.size > size) return false;
    return true;
}

// the game in assets, as TSFP.exe takes it: default.xbe and its folders first, then the disc images by name
Check Scan() {
    Check wrong, broken;
    std::error_code ec;
    fs::path assets = Assets();
    auto seen = [&](Check& keep, const std::wstring& name, std::wstring detail) {
        if (keep.name.empty()) keep = {Found::Missing, name, std::move(detail)};
    };
    std::vector<uint8_t> xbe;
    if (ReadFile(assets / kXbe, xbe)) {
        if (Sha1(xbe) != kImageSha1)
            seen(wrong, kXbe, L"its default.xbe is another release");
        else if (!fs::is_directory(assets / "pak", ec) || !fs::is_directory(assets / "xmv", ec))
            seen(broken, kXbe, L"the pak or xmv folder beside default.xbe is missing");
        else
            return {Found::Ready, L"default.xbe and its folders", {}};
    }
    std::vector<fs::path> isos;
    for (auto& e : fs::directory_iterator(assets, ec))
        if (Lower(e.path().extension().wstring()) == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
    std::sort(isos.begin(), isos.end());
    for (const fs::path& iso : isos) {
        std::wstring name = iso.filename().wstring();
        kernel::Disc disc;
        std::string error;
        if (!disc.Open(iso, error)) {
            seen(broken, name, L"it is no Xbox disc image (" + Widen(error) + L")");
            continue;
        }
        if (!disc.Load("default.xbe", xbe)) {
            seen(broken, name, L"it has no default.xbe");
            continue;
        }
        if (Sha1(xbe) != kImageSha1) {
            seen(wrong, name, L"it is another release of the game");
            continue;
        }
        if (!disc.Find("pak") || !disc.Find("xmv") || !Whole(*disc.Find(""), fs::file_size(iso, ec))) {
            seen(broken, name, L"the image is cut short or damaged");
            continue;
        }
        return {Found::Ready, name, {}};
    }
    if (!broken.name.empty()) {
        broken.found = Found::Broken;
        return broken;
    }
    if (!wrong.name.empty()) {
        wrong.found = Found::Wrong;
        return wrong;
    }
    return {Found::Missing, {}, {}};
}

// ---- what the launcher is doing, shared with its worker ----------------------------------------------------------

enum class Upd { Off, Checking, Current, Available, Working, Failed };
struct Status {
    Check check{Found::Checking};
    bool busy = false;
    float progress = -1;  // 0-1 while copying
    std::wstring note;    // the last import's outcome, or an error
    bool error = false;
    // the updates
    Upd upd = Upd::Off;
    std::wstring version, latest, upd_note;  // installed, the latest release's, what the update is doing
    std::string url;                         // the latest release's TSFP.zip
    float upd_progress = -1;
};
std::mutex g_mutex;
Status g_status;
std::atomic<bool> g_cancel{false};
HWND g_window = nullptr;
bool g_snapshot = false;

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    return g_status;
}
template <class F>
void Update(F&& f) {
    std::lock_guard lock(g_mutex);
    f(g_status);
}

void Rescan(std::wstring note = {}, bool error = false) {
    Check c = Scan();
    Update([&](Status& s) {
        s.check = c;
        s.busy = false;
        s.progress = -1;
        s.note = std::move(note);
        s.error = error;
    });
}

std::wstring Size(uint64_t bytes) {
    wchar_t text[32];
    swprintf(text, 32, L"%.1f GB", bytes / 1073741824.0);
    return text;
}

// copies, or hard links when on the same drive (at once, no more space): what was done, or "" when cancelled or failed
struct Copier {
    uint64_t done = 0, total = 0;
    bool copied = false;
    static DWORD CALLBACK Progress(LARGE_INTEGER, LARGE_INTEGER moved, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                   HANDLE, HANDLE, LPVOID self) {
        auto* c = static_cast<Copier*>(self);
        float p = c->total ? float(double(c->done + moved.QuadPart) / double(c->total)) : 0;
        Update([&](Status& s) { s.progress = p; });
        return g_cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
    }
    bool File(const fs::path& from, const fs::path& to, uint64_t size) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);
        if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) {
            done += size;
            return true;
        }
        copied = true;
        BOOL cancel = FALSE;
        if (!CopyFileExW(from.c_str(), to.c_str(), Progress, this, &cancel, COPY_FILE_RESTARTABLE)) {
            fs::remove(to, ec);
            return false;
        }
        done += size;
        return true;
    }
};

// the import, on its own thread: one disc image, or the folder of a default.xbe
void Import(fs::path from) {
    std::error_code ec;
    fs::path assets = Assets();
    fs::create_directories(assets, ec);
    Copier copier;
    std::vector<std::pair<fs::path, uint64_t>> files;
    fs::path base = from.parent_path();
    bool iso = Lower(from.extension().wstring()) == L".iso";
    if (iso) {
        files.push_back({from, fs::file_size(from, ec)});
    } else {
        for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back({it->path(), it->file_size(ec)});
    }
    for (auto& f : files) copier.total += f.second;

    // room for it, unless every file links
    ULARGE_INTEGER free{};
    GetDiskFreeSpaceExW(assets.c_str(), &free, nullptr, nullptr);
    wchar_t from_root[MAX_PATH] = {}, to_root[MAX_PATH] = {};
    GetVolumePathNameW(from.c_str(), from_root, MAX_PATH);
    GetVolumePathNameW(assets.c_str(), to_root, MAX_PATH);
    if (_wcsicmp(from_root, to_root) != 0 && free.QuadPart < copier.total) {
        Rescan(L"Not enough space for the copy: " + Size(copier.total) + L" needed, " + Size(free.QuadPart) +
                   L" free where the launcher is.",
               true);
        return;
    }

    for (auto& [file, size] : files) {
        fs::path to = assets / (iso ? file.filename() : file.lexically_relative(base));
        if (!copier.File(file, to, size)) {
            Rescan(g_cancel ? L"Import cancelled." : L"Could not copy " + file.filename().wstring() + L".", !g_cancel);
            return;
        }
    }
    Rescan(copier.copied ? L"Imported: copied into the assets folder." : L"Imported: linked into the assets folder, no space used.");
}

void StartImport(const fs::path& from) {
    if (GetStatus().busy) return;
    std::error_code ec;
    fs::path assets = Assets();
    bool iso = Lower(from.extension().wstring()) == L".iso";
    if (!iso && Lower(from.filename().wstring()) != kXbe) {
        Update([](Status& s) { s.note = L"Choose the game's disc image (.iso) or its default.xbe.", s.error = true; });
        return;
    }
    // the right release first: the xbe alone is quick to read, from the image as well
    std::vector<uint8_t> xbe;
    std::string error;
    kernel::Disc disc;
    bool read = iso ? disc.Open(from, error) && disc.Load("default.xbe", xbe) : ReadFile(from, xbe);
    if (!read) {
        Update([](Status& s) { s.note = L"That file is no Xbox disc image of the game.", s.error = true; });
        return;
    }
    if (Sha1(xbe) != kImageSha1 &&
        MessageBoxW(g_window,
                    L"This is another release of TimeSplitters Future Perfect than TSFP.exe was made from (the US Xbox "
                    L"release), and will most likely crash.\n\nImport it anyway?",
                    L"TSFP", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    if (fs::equivalent(from.parent_path(), assets, ec)) {
        Update([](Status& s) { s.busy = true; });
        std::thread([] { Rescan(L"That copy is already in the assets folder."); }).detach();
        return;
    }
    g_cancel = false;
    Update([](Status& s) { s.busy = true, s.progress = 0, s.note.clear(), s.error = false; });
    std::thread(Import, from).detach();
}

// ---- drawing ---------------------------------------------------------------------------------------------------

ComPtr<ID2D1Factory1> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<IWICImagingFactory> g_wic;

struct Fonts {
    ComPtr<IDWriteTextFormat> heading, title, body, note, label, button, play, bar, tagline;
} g_fonts;

ComPtr<IDWriteTextFormat> Font(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                               DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL) {
    ComPtr<IDWriteTextFormat> f;
    g_dwrite->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    return f;
}

// the title's letters as a shape, to fill with chrome and outline: tracked, its bounds out
ComPtr<ID2D1PathGeometry> TextShape(const wchar_t* text, const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                                    float size, float tracking) {
    ComPtr<IDWriteFontCollection> fonts;
    g_dwrite->GetSystemFontCollection(&fonts);
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(family, &index, &exists);
    if (!exists) fonts->FindFamilyName(L"Georgia", &index, &exists);
    ComPtr<IDWriteFontFamily> fam;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    fonts->GetFontFamily(index, &fam);
    fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
    font->CreateFontFace(&face);
    DWRITE_FONT_METRICS fm;
    face->GetMetrics(&fm);
    std::vector<UINT32> chars(text, text + wcslen(text));
    std::vector<UINT16> glyphs(chars.size());
    face->GetGlyphIndicesW(chars.data(), UINT32(chars.size()), glyphs.data());
    std::vector<DWRITE_GLYPH_METRICS> gm(glyphs.size());
    face->GetDesignGlyphMetrics(glyphs.data(), UINT32(glyphs.size()), gm.data(), FALSE);
    std::vector<float> advances(glyphs.size());
    for (size_t i = 0; i < glyphs.size(); i++) advances[i] = gm[i].advanceWidth * size / fm.designUnitsPerEm + tracking;
    ComPtr<ID2D1PathGeometry> shape;
    ComPtr<ID2D1GeometrySink> sink;
    g_d2d->CreatePathGeometry(&shape);
    shape->Open(&sink);
    face->GetGlyphRunOutline(size, glyphs.data(), advances.data(), nullptr, UINT32(glyphs.size()), FALSE, FALSE, sink.Get());
    sink->Close();
    return shape;
}

// smooth value noise for the portal's swirl
float Hash(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h ^ (h >> 16)) / 4294967295.f;
}
float Noise(float x, float y) {
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
float Fbm(float x, float y) {
    float v = 0, amp = 0.5f;
    for (int i = 0; i < 4; i++, x *= 2.03f, y *= 2.03f, amp *= 0.5f) v += amp * Noise(x, y);
    return v;
}
float Smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// the portal's swirl: spiral arms of light, its color added on what is behind
std::vector<uint32_t> Swirl(int size, int arms, float twist, float seed) {
    std::vector<uint32_t> px(size * size);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1, r = std::sqrt(dx * dx + dy * dy);
            if (r >= 1) continue;
            float th = std::atan2(dy, dx);
            float n = Fbm(dx * 3.1f + seed, dy * 3.1f - seed);
            float phase = th * arms - std::log(r + 0.03f) * twist + n * 3.0f;
            float a = std::pow(0.5f + 0.5f * std::cos(phase), 3.f);
            float fine = std::pow(0.5f + 0.5f * std::cos(phase * 2.6f + n * 6), 12.f) * 0.6f;
            float v = (a * (0.25f + 0.9f * n) + fine) * Smooth(1.0f, 0.5f, r) * (0.4f + 0.8f * Smooth(1.0f, 0.15f, r));
            v += 0.8f * std::exp(-r * r * 22);
            v = std::clamp(v, 0.f, 1.f);
            // deep blue, to cyan, to white
            float t = Smooth(0, 0.55f, v), w = Smooth(0.55f, 1.0f, v);
            float R = (0.04f + (0.30f - 0.04f) * t) * (1 - w) + w, G = (0.12f + (0.78f - 0.12f) * t) * (1 - w) + w,
                  B = (0.50f + (1.0f - 0.50f) * t) * (1 - w) + w;
            auto c = [&](float f) { return uint32_t(std::clamp(f * v, 0.f, 1.f) * 255 + 0.5f); };
            px[y * size + x] = c(1) << 24 | c(R) << 16 | c(G) << 8 | c(B);
        }
    return px;
}

struct Star {
    float x, y, size, phase;
};
struct Streak {
    float angle, dist, start, speed, width;
    bool warm;
};

struct Button {
    D2D1_RECT_F rect;
    const wchar_t* text;
    bool primary = false;
};
enum ButtonId { kImport, kPlay, kVerify, kFolder, kUpdate, kMinimize, kClose, kButtons };

struct Gfx {
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext> dc;  // for added light
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> swirl, swirl_fine, icon;
    ComPtr<ID2D1Bitmap> cover, logo;  // art/cover.jpg and art/logo.png, when there
    std::vector<ComPtr<ID2D1Bitmap>> slides;  // art/slides, shown after the cover in turn
    ComPtr<ID2D1LinearGradientBrush> cover_fade;
    ComPtr<ID2D1LinearGradientBrush> streak, chrome, sky, shade, play;
    ComPtr<ID2D1RadialGradientBrush> glow, core;
    ComPtr<ID2D1PathGeometry> title, tagline_line;
    D2D1_RECT_F title_bounds{};
};

// the pages under the title: playing (the disc, updates), the settings the game reads at start, the mods, the saves
enum Page { kPagePlay, kPageSettings, kPageMods, kPageSaves, kPages };
constexpr const wchar_t* kPageNames[kPages] = {L"PLAY", L"SETTINGS", L"MODS", L"SAVES"};
constexpr float kShift = 8;  // the play page under the page names

// a place to click a page drew this frame, after the fixed buttons (its id kButtons and on)
struct Hot {
    D2D1_RECT_F rect;
    std::function<void()> press;
};

struct Ui {
    Button buttons[kButtons];
    int page = kPagePlay;
    std::vector<Hot> hot;
    int hover = -1, pressed = -1;
    std::vector<Star> stars;
    std::vector<Streak> streaks;
    std::mt19937 rng{20050321};
    double time = 0;
    float spawn = 0;
} g_ui;

float Random(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_ui.rng); }

ComPtr<ID2D1GradientStopCollection> Stops(ID2D1RenderTarget* rt, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
    ComPtr<ID2D1GradientStopCollection> c;
    rt->CreateGradientStopCollection(stops.begin(), UINT32(stops.size()), &c);
    return c;
}

ComPtr<ID2D1Bitmap> Bitmap(ID2D1RenderTarget* rt, const std::vector<uint32_t>& px, int size) {
    ComPtr<ID2D1Bitmap> b;
    rt->CreateBitmap(D2D1::SizeU(size, size), px.data(), size * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    return b;
}

// a picture file as a bitmap, none when it is missing or not read; read whole first, so the file is not held open
// (an update replaces it)
ComPtr<ID2D1Bitmap> Picture(ID2D1RenderTarget* rt, const fs::path& file) {
    ComPtr<ID2D1Bitmap> b;
    std::vector<uint8_t> data;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (ReadFile(file, data) && !data.empty() && SUCCEEDED(g_wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(data.data(), DWORD(data.size()))) &&
        SUCCEEDED(g_wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), &b);
    return b;
}

void CreateResources(Gfx& g) {
    ID2D1RenderTarget* rt = g.rt.Get();
    g.rt.As(&g.dc);
    rt->CreateSolidColorBrush(kText, &g.brush);
    static const std::vector<uint32_t> swirl = Swirl(512, 3, 4.2f, 1.7f), fine = Swirl(256, 5, 7.5f, 5.3f);
    g.swirl = Bitmap(rt, swirl, 512);
    g.swirl_fine = Bitmap(rt, fine, 256);
    rt->CreateLinearGradientBrush({}, Stops(rt, {{0, Rgb(0x52C8FF, 0)}, {0.7f, Rgb(0x9FE3FF, 0.7f)}, {1, Rgb(0xFFFFFF, 1)}}).Get(), &g.streak);
    rt->CreateLinearGradientBrush({}, Stops(rt, {{0, Rgb(0xFFFFFF)}, {0.38f, Rgb(0xD5DEEA)}, {0.5f, Rgb(0x56657C)},
                                                 {0.56f, Rgb(0xF3F7FC)}, {0.8f, Rgb(0xA9B6C8)}, {1, Rgb(0x6F7D92)}}).Get(), &g.chrome);
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, kBar}, {0, kHeight}),
                                  Stops(rt, {{0, Rgb(0x03050B)}, {0.45f, Rgb(0x0A1A38)}, {1, Rgb(0x03050A)}}).Get(), &g.sky);
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, kHeight - 230}, {0, kHeight}),
                                  Stops(rt, {{0, Rgb(0x03050A, 0)}, {0.55f, Rgb(0x03050A, 0.85f)}, {1, Rgb(0x03050A, 1)}}).Get(), &g.shade);
    rt->CreateLinearGradientBrush({}, Stops(rt, {{0, Rgb(0xF0702C)}, {1, Rgb(0xB8400F)}}).Get(), &g.play);
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({}, {}, 1, 1),
                                  Stops(rt, {{0, Rgb(0x2A6BD8, 0.55f)}, {0.5f, Rgb(0x14306E, 0.25f)}, {1, Rgb(0x0A1A38, 0)}}).Get(), &g.glow);
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({}, {}, 1, 1),
                                  Stops(rt, {{0, Rgb(0xFFFFFF, 1)}, {0.35f, Rgb(0xBFF0FF, 0.7f)}, {1, Rgb(0x52C8FF, 0)}}).Get(), &g.core);
    g.cover = Picture(rt, ExeDir() / "art" / "cover.jpg");
    g.logo = Picture(rt, ExeDir() / "art" / "logo.png");
    g.slides.clear();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ExeDir() / "art" / "slides", ec)) {
        std::wstring ext = Lower(e.path().extension().wstring());
        if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
        if (ComPtr<ID2D1Bitmap> b = Picture(rt, f)) g.slides.push_back(b);
    rt->CreateLinearGradientBrush({}, Stops(rt, {{0, Rgb(0x05070C, 0)}, {1, Rgb(0x05070C, 1)}}).Get(), &g.cover_fade);
    g.title = TextShape(L"TIMESPLITTERS", L"Palatino Linotype", DWRITE_FONT_WEIGHT_BOLD, 46, 1.5f);
    g.title->GetBounds(nullptr, &g.title_bounds);

    // the exe's icon, top right
    HICON icon = HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
    ComPtr<IWICBitmap> wic;
    ComPtr<IWICFormatConverter> conv;
    if (icon && SUCCEEDED(g_wic->CreateBitmapFromHICON(icon, &wic)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wic.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), &g.icon);
    if (icon) DestroyIcon(icon);
}

void Fill(Gfx& g, D2D1_RECT_F r, D2D1_COLOR_F c) {
    g.brush->SetColor(c);
    g.rt->FillRectangle(r, g.brush.Get());
}
void Text(Gfx& g, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float tracking = 0,
          DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    ComPtr<IDWriteTextLayout> layout;
    g_dwrite->CreateTextLayout(s.c_str(), UINT32(s.size()), f, r.right - r.left, r.bottom - r.top, &layout);
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(valign);
    ComPtr<IDWriteTextLayout1> spaced;
    if (tracking && SUCCEEDED(layout.As(&spaced))) spaced->SetCharacterSpacing(0, tracking, 0, {0, UINT32(s.size())});
    g.brush->SetColor(c);
    g.rt->DrawTextLayout({r.left, r.top}, layout.Get(), g.brush.Get());
}
void Added(Gfx& g, bool on) {
    if (g.dc) g.dc->SetPrimitiveBlend(on ? D2D1_PRIMITIVE_BLEND_ADD : D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

// ---- the banner: a time portal, light streaking out of it, and the title in chrome --------------------------------

constexpr D2D1_POINT_2F kPortal = {kBanner / 2, kBar + 262};
constexpr float kPortalX = 128, kPortalY = 186, kTilt = -9;

void Simulate(float dt) {
    if (g_ui.stars.empty())
        for (int i = 0; i < 150; i++)
            g_ui.stars.push_back({Random(0, kBanner), Random(kBar, kHeight), Random(0.4f, 1.6f), Random(0, 6.28f)});
    for (Streak& s : g_ui.streaks) s.dist += dt * s.speed * (1 + s.dist / 55);
    std::erase_if(g_ui.streaks, [](const Streak& s) { return s.dist > 700; });
    for (g_ui.spawn += dt * 46; g_ui.spawn >= 1; g_ui.spawn--) {
        float d = Random(20, 90);
        g_ui.streaks.push_back({Random(0, 2 * kPi), d, d, Random(22, 48), Random(0.6f, 2.0f), Random(0, 1) < 0.12f});
    }
}

bool Glitching(double t) { return std::fmod(t, 7.0) > 6.75; }

// a picture filling the banner, its middle kept, drifting slowly in (zoom)
void DrawFilling(Gfx& g, ID2D1Bitmap* b, float zoom, float opacity) {
    D2D1_SIZE_F size = b->GetSize();
    float scale = std::max(kBanner / size.width, (kHeight - kBar) / size.height) * zoom;
    float w = size.width * scale, h = size.height * scale, cy = (kBar + kHeight) / 2;
    g.rt->DrawBitmap(b, {(kBanner - w) / 2, cy - h / 2, (kBanner + w) / 2, cy + h / 2}, opacity,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// the cover filling the banner, drifting slowly in as the game's own menus do; a cover wider than tall goes across the
// top, fading into the dark, with the logo (art/logo.png) under it. With slides, the cover and each slide shown
// kSlide seconds in turn, the last kFade of them fading into the next, each drifting in from its start
constexpr double kSlide = 9, kFade = 1.6;
void DrawCover(Gfx& g, double t) {
    ID2D1RenderTarget* rt = g.rt.Get();
    D2D1_RECT_F area = {0, kBar, kBanner, kHeight};
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    Fill(g, area, Rgb(0x05070C));
    D2D1_SIZE_F size = g.cover->GetSize();
    float zoom = 1.03f + 0.02f * std::sin(float(t) * 0.25f);
    size_t count = g.slides.size() + 1, at = size_t(t / kSlide) % count;
    double into = std::fmod(t, kSlide);
    auto picture = [&](size_t i) { return i ? g.slides[i - 1].Get() : g.cover.Get(); };
    auto drift = [&](double since) { return 1.02f + 0.05f * float(std::min(since, kSlide + kFade) / (kSlide + kFade)); };
    if (count > 1 && (at || size.height >= size.width)) {
        // the slide shown, then the next fading in over it
        DrawFilling(g, picture(at), drift(into + kFade), 1);
        if (into > kSlide - kFade) {
            float f = float((into - (kSlide - kFade)) / kFade);
            DrawFilling(g, picture((at + 1) % count), drift(into - (kSlide - kFade)), f * f * (3 - 2 * f));
        }
    } else if (size.height >= size.width) {
        DrawFilling(g, g.cover.Get(), zoom, 1);
    } else {
        float height = kBanner * size.height / size.width;
        float w = kBanner * zoom, h = height * zoom;
        rt->DrawBitmap(g.cover.Get(), {(kBanner - w) / 2, kBar + (height - h) / 2, (kBanner + w) / 2, kBar + (height + h) / 2},
                       1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.cover_fade->SetStartPoint({0, kBar + height - 170});
        g.cover_fade->SetEndPoint({0, kBar + height});
        rt->FillRectangle({0, kBar + height - 170, kBanner, kBar + height + 1}, g.cover_fade.Get());
        if (g.logo) {
            D2D1_SIZE_F l = g.logo->GetSize();
            float lw = kBanner - 56, lh = lw * l.height / l.width;
            float top = std::min(kBar + height - lh * 0.2f, kHeight - 40 - lh);
            rt->DrawBitmap(g.logo.Get(), {28, top, 28 + lw, top + lh}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    rt->PopAxisAlignedClip();
    g.brush->SetColor(Rgb(0x52C8FF, 0.35f));
    rt->DrawLine({kBanner, kBar}, {kBanner, kHeight}, g.brush.Get(), 1);
}

void DrawBanner(Gfx& g, double t) {
    if (g.cover) return DrawCover(g, t);
    ID2D1RenderTarget* rt = g.rt.Get();
    D2D1_RECT_F area = {0, kBar, kBanner, kHeight};
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->FillRectangle(area, g.sky.Get());
    bool glitch = Glitching(t);
    float pulse = 0.5f + 0.5f * std::sin(float(t) * 1.7f), flare = glitch ? 1.0f : 0.0f;

    // stars, twinkling, slowly drawn toward the portal
    for (const Star& s : g_ui.stars) {
        float a = 0.25f + 0.55f * (0.5f + 0.5f * std::sin(float(t) * 1.3f + s.phase));
        g.brush->SetColor(Rgb(0xCFE6FF, a * 0.8f));
        rt->FillEllipse(D2D1::Ellipse({s.x, s.y}, s.size * 0.6f, s.size * 0.6f), g.brush.Get());
    }

    Added(g, true);
    // the glow the portal throws around it
    g.glow->SetCenter(kPortal);
    g.glow->SetRadiusX(kPortalX * 2.4f);
    g.glow->SetRadiusY(kPortalY * 2.0f);
    g.glow->SetOpacity(0.8f + 0.2f * pulse + flare * 0.5f);
    rt->FillRectangle(area, g.glow.Get());

    // streaks of light flying out of the portal toward the viewer
    for (const Streak& s : g_ui.streaks) {
        float cs = std::cos(s.angle), sn = std::sin(s.angle);
        float len = 10 + (s.dist - s.start) * 0.55f;
        D2D1_POINT_2F head = {kPortal.x + cs * s.dist * 0.95f, kPortal.y + sn * s.dist * 1.15f};
        D2D1_POINT_2F tail = {kPortal.x + cs * (s.dist - len) * 0.95f, kPortal.y + sn * (s.dist - len) * 1.15f};
        float fade = std::clamp((s.dist - s.start) / 50, 0.f, 1.f);
        g.streak->SetStartPoint(tail);
        g.streak->SetEndPoint(head);
        g.streak->SetOpacity(fade * (s.warm ? 0.55f : 0.8f));
        rt->DrawLine(tail, head, g.streak.Get(), s.width * (1 + s.dist / 260));
        if (s.warm) {
            g.brush->SetColor(Rgb(0xFF9A4A, fade * 0.7f));
            rt->DrawLine(tail, head, g.brush.Get(), s.width * 0.6f);
        }
    }

    // the portal: two swirls turning against each other, a tilted oval seen from aside
    auto layer = [&](ID2D1Bitmap* bmp, float spin, float sx, float sy, float opacity) {
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(spin) * D2D1::Matrix3x2F::Scale(sx, sy) *
                         D2D1::Matrix3x2F::Rotation(kTilt) * D2D1::Matrix3x2F::Translation(kPortal.x, kPortal.y));
        rt->DrawBitmap(bmp, {-1, -1, 1, 1}, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    };
    layer(g.swirl.Get(), float(t) * 38, kPortalX * 1.08f, kPortalY * 1.08f, 0.95f);
    layer(g.swirl_fine.Get(), -float(t) * 61, kPortalX * 0.86f, kPortalY * 0.86f, 0.6f + 0.2f * pulse);
    layer(g.swirl.Get(), float(t) * 97 + 40, kPortalX * 0.42f, kPortalY * 0.42f, 0.7f);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(kTilt, kPortal));
    // its rim, glowing
    D2D1_ELLIPSE rim = D2D1::Ellipse(kPortal, kPortalX, kPortalY);
    const float widths[] = {22, 12, 6, 2.5f, 1.2f}, alphas[] = {0.05f, 0.09f, 0.16f, 0.45f, 0.9f};
    for (int i = 0; i < 5; i++) {
        g.brush->SetColor(Rgb(i < 3 ? 0x52C8FF : 0xDDF6FF, alphas[i] * (0.85f + 0.15f * pulse + flare * 0.4f)));
        rt->DrawEllipse(rim, g.brush.Get(), widths[i]);
    }
    // its white-hot heart
    float heart = 46 + 8 * pulse + flare * 30;
    g.core->SetCenter(kPortal);
    g.core->SetRadiusX(heart);
    g.core->SetRadiusY(heart * 1.35f);
    rt->FillEllipse(D2D1::Ellipse(kPortal, heart, heart * 1.35f), g.core.Get());
    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // now and then time splits: a bolt across the portal
    if (glitch) {
        std::mt19937 bolt(uint32_t(t * 30));
        auto rnd = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(bolt); };
        D2D1_POINT_2F p = {kPortal.x + rnd(-60, 60), kPortal.y - kPortalY - 20};
        for (int i = 0; i < 14; i++) {
            D2D1_POINT_2F q = {p.x + rnd(-26, 26), p.y + (2 * kPortalY + 40) / 14};
            g.brush->SetColor(Rgb(0x52C8FF, 0.35f));
            rt->DrawLine(p, q, g.brush.Get(), 7);
            g.brush->SetColor(Rgb(0xFFFFFF, 0.95f));
            rt->DrawLine(p, q, g.brush.Get(), 1.6f);
            p = q;
        }
    }
    Added(g, false);

    // the title, over a shade at the foot
    rt->FillRectangle(area, g.shade.Get());
    D2D1_RECT_F b = g.title_bounds;
    float scale = std::min(1.f, (kBanner - 52) / (b.right - b.left));
    float x = (kBanner - (b.right - b.left) * scale) / 2 - b.left * scale, y = kHeight - 128 - b.top * scale;
    auto at = [&](float dx) {
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x + dx, y));
    };
    at(0);
    Added(g, true);
    g.brush->SetColor(Rgb(0x52C8FF, 0.10f + 0.06f * pulse));
    rt->DrawGeometry(g.title.Get(), g.brush.Get(), 10);
    Added(g, false);
    g.brush->SetColor(Rgb(0x05080F));
    rt->DrawGeometry(g.title.Get(), g.brush.Get(), 4);
    if (glitch) {
        // split in time: copies of the title out of step, red and blue
        Added(g, true);
        float off = 3 + 4 * std::fabs(std::sin(float(t) * 90));
        at(-off);
        g.brush->SetColor(Rgb(0xFF3B3B, 0.75f));
        rt->FillGeometry(g.title.Get(), g.brush.Get());
        at(off);
        g.brush->SetColor(Rgb(0x3BB8FF, 0.75f));
        rt->FillGeometry(g.title.Get(), g.brush.Get());
        Added(g, false);
        at(0);
    }
    g.chrome->SetStartPoint({0, b.top});
    g.chrome->SetEndPoint({0, b.bottom});
    rt->FillGeometry(g.title.Get(), g.chrome.Get());
    // a glint passing over the chrome
    float glint = float(std::fmod(t, 5.0)) / 1.2f;
    if (glint < 1) {
        float gx = b.left + (b.right - b.left + 120) * glint - 60;
        ComPtr<ID2D1LinearGradientBrush> shine;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({gx - 40, b.top}, {gx + 40, b.bottom}),
                                      Stops(rt, {{0, Rgb(0xFFFFFF, 0)}, {0.5f, Rgb(0xFFFFFF, 0.85f)}, {1, Rgb(0xFFFFFF, 0)}}).Get(), &shine);
        rt->FillGeometry(g.title.Get(), shine.Get());
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // FUTURE PERFECT between two bolts
    float ty = y + b.bottom * scale + 14;
    Text(g, L"FUTURE PERFECT", g_fonts.tagline.Get(), {0, ty, kBanner, ty + 24}, Rgb(0xE6ECF5), DWRITE_TEXT_ALIGNMENT_CENTER, 5);
    auto bolt = [&](float from, float to, float dir) {
        D2D1_POINT_2F pts[] = {{from, ty + 12}, {from + dir * (to - from) * 0.45f, ty + 12}, {from + dir * (to - from) * 0.52f, ty + 7},
                               {from + dir * (to - from) * 0.58f, ty + 15}, {from + dir * (to - from) * 0.64f, ty + 12}, {to, ty + 12}};
        for (int i = 0; i < 5; i++) {
            g.brush->SetColor(Rgb(0x52C8FF, 0.9f));
            rt->DrawLine(pts[i], pts[i + 1], g.brush.Get(), 1.4f);
        }
    };
    bolt(34, 112, 1);
    bolt(kBanner - 34, kBanner - 112, 1);

    rt->PopAxisAlignedClip();
    // the banner's edge
    g.brush->SetColor(Rgb(0x52C8FF, 0.35f));
    rt->DrawLine({kBanner, kBar}, {kBanner, kHeight}, g.brush.Get(), 1);
}

// ---- the page beside it ----------------------------------------------------------------------------------------

void Layout() {
    float l = kBanner + 42, r = kWidth - 42, mid = (l + r) / 2;
    g_ui.buttons[kImport] = {{l, 447 + kShift, mid - 8, 499 + kShift}, L"IMPORT / REPAIR"};
    g_ui.buttons[kPlay] = {{mid + 8, 447 + kShift, r, 499 + kShift}, L"PLAY", true};
    g_ui.buttons[kVerify] = {{l, 511 + kShift, mid - 8, 549 + kShift}, L"VERIFY FILES"};
    g_ui.buttons[kFolder] = {{mid + 8, 511 + kShift, r, 549 + kShift}, L"OPEN GAME FOLDER"};
    g_ui.buttons[kUpdate] = {{mid + 8, 593 + kShift, r, 631 + kShift}, L"CHECK FOR UPDATES"};
    g_ui.buttons[kMinimize] = {{kWidth - 92, 0, kWidth - 46, kBar}, L""};
    g_ui.buttons[kClose] = {{kWidth - 46, 0, kWidth, kBar}, L""};
}

bool Enabled(int id, const Status& s) {
    bool ready = s.check.found == Found::Ready && !s.busy;
    switch (id) {
    case kImport: return !s.busy || s.progress >= 0;
    case kPlay: return ready;
    case kUpdate: return !s.busy && s.upd != Upd::Off && s.upd != Upd::Checking && s.upd != Upd::Working;
    case kVerify: return !s.busy;
    default: return true;
    }
}

void DrawPlay(Gfx& g, const Status& s) {
    ID2D1RenderTarget* rt = g.rt.Get();
    float l = kBanner + 42, r = kWidth - 42;
    D2D1_MATRIX_3X2_F was;
    rt->GetTransform(&was);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, kShift) * was);

    std::wstring heading, body;
    switch (s.busy && s.progress >= 0 ? Found::Checking : s.check.found) {
    case Found::Checking:
        heading = s.busy && s.progress >= 0 ? L"CROSSING THE PORTAL" : L"OPENING THE PORTAL";
        body = s.busy && s.progress >= 0 ? L"Bringing your game into the assets folder. It stays yours: nothing leaves this PC."
                                         : L"Looking for your game in the assets folder\x2026";
        break;
    case Found::Missing:
        heading = L"THE PORTAL IS CLOSED";
        body = L"Import your own copy of TimeSplitters Future Perfect for Xbox (US): its disc image (.iso), or the "
               L"default.xbe of its extracted files. You can also drop it on this window.";
        break;
    case Found::Wrong:
        heading = L"WRONG TIMELINE";
        body = s.check.name + L": " + s.check.detail + L". This build runs the US Xbox release; import that one.";
        break;
    case Found::Broken:
        heading = L"THE PORTAL IS DAMAGED";
        body = s.check.name + L": " + s.check.detail + L". Import the game again to repair it.";
        break;
    case Found::Ready:
        heading = L"TIME TO SPLIT";
        body = L"Your copy is ready. Press F1 in the game for graphics, controls and co-op settings.";
        break;
    }
    Text(g, heading, g_fonts.title.Get(), {l, 152, r, 186}, kText, DWRITE_TEXT_ALIGNMENT_LEADING, 1.5f);
    Text(g, body, g_fonts.body.Get(), {l, 192, r, 262}, Rgb(0xC4C8D2));

    // what this is
    D2D1_RECT_F box = {l, 272, r, 362};
    Fill(g, box, kBox);
    g.brush->SetColor(kLine);
    rt->DrawRectangle(box, g.brush.Get(), 1);
    Text(g, L"YOUR DISC, RECOMPILED", g_fonts.label.Get(), {l + 18, 287, r, 305}, kCyan, DWRITE_TEXT_ALIGNMENT_LEADING, 1.5f);
    Text(g, L"Native PC \x00B7 Any resolution \x00B7 60 FPS \x00B7 Mouse & keyboard \x00B7 System Link online",
         g_fonts.body.Get(), {l + 18, 309, r - 12, 331}, kText);
    Text(g, L"Original game files are not included.", g_fonts.note.Get(), {l + 18, 334, r, 352}, kDim);

    // where it stands
    std::wstring line;
    D2D1_COLOR_F color = kDim;
    if (s.busy && s.progress >= 0) {
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", int(s.progress * 100));
        line = std::wstring(L"Importing\x2026 ") + pct;
    } else if (!s.note.empty()) {
        line = s.note;
        color = s.error ? kRed : kCyan;
    } else if (s.check.found == Found::Ready) {
        line = L"Installed: " + s.check.name + L". Files are verified before every launch.";
    } else if (s.check.found == Found::Checking) {
        line = L"Checking\x2026";
    } else {
        line = L"Not installed.";
    }
    Text(g, line, g_fonts.note.Get(), {l, 404, r, 422}, color);
    Fill(g, {l, 428, r, 432}, Rgb(0x1E232D));
    if (s.busy && s.progress > 0) {
        g.play->SetStartPoint({l, 0});
        g.play->SetEndPoint({r, 0});
        g.play->SetOpacity(1);
        rt->FillRectangle({l, 428, l + (r - l) * s.progress, 432}, g.play.Get());
    } else if (s.check.found == Found::Ready && !s.busy) {
        Fill(g, {l, 428, r, 432}, Rgb(0x52C8FF, 0.75f));
    }

    rt->SetTransform(was);
    for (int id = 0; id < kMinimize; id++) {
        const Button& b = g_ui.buttons[id];
        bool on = Enabled(id, s), hover = on && g_ui.hover == id, down = hover && g_ui.pressed == id;
        const wchar_t* text = id == kImport && s.busy && s.progress >= 0 ? L"CANCEL" : b.text;
        if (id == kUpdate) text = s.upd == Upd::Available ? L"UPDATE NOW" : s.upd == Upd::Working ? L"UPDATING\x2026" : b.text;
        if (b.primary && on) {
            g.play->SetStartPoint({0, b.rect.top});
            g.play->SetEndPoint({0, b.rect.bottom});
            g.play->SetOpacity(down ? 0.8f : 1);
            rt->FillRectangle(b.rect, g.play.Get());
            if (hover) Fill(g, b.rect, Rgb(0xFFFFFF, down ? 0 : 0.10f));
            g.brush->SetColor(Rgb(0xFF9A5A, 0.8f));
        } else {
            Fill(g, b.rect, b.primary ? Rgb(0x3A2018) : hover ? Rgb(0x232A37) : kButton);
            g.brush->SetColor(hover ? Rgb(0x52C8FF, 0.8f) : Rgb(0x343B49));
        }
        rt->DrawRectangle(b.rect, g.brush.Get(), 1);
        Text(g, text, b.primary ? g_fonts.play.Get() : g_fonts.button.Get(), b.rect,
             on ? (b.primary ? Rgb(0xFFFFFF) : kText) : kFaint, DWRITE_TEXT_ALIGNMENT_CENTER, 1, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // the updates
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, kShift) * was);
    float mid = (l + r) / 2;
    Text(g, L"UPDATES", g_fonts.label.Get(), {l, 567, r, 585}, kCyan, DWRITE_TEXT_ALIGNMENT_LEADING, 1.5f);
    std::wstring installed = L"Installed: " + (s.version.empty() ? std::wstring(L"unknown") : s.version);
    Text(g, installed, g_fonts.body.Get(), {l, 593, mid - 8, 612}, kText);
    std::wstring state;
    D2D1_COLOR_F state_color = kDim;
    switch (s.upd) {
    case Upd::Off: state = L"No update source in this release."; break;
    case Upd::Checking: state = L"Looking for a new version\x2026"; break;
    case Upd::Current: state = L"Up to date."; break;
    case Upd::Available: state = L"New version: " + s.latest, state_color = kCyan; break;
    case Upd::Working: state = s.upd_note, state_color = kCyan; break;
    case Upd::Failed: state = s.upd_note, state_color = kRed; break;
    }
    Text(g, state, g_fonts.note.Get(), {l, 614, mid - 8, 632}, state_color);
    if (s.upd == Upd::Working && s.upd_progress >= 0) {
        Fill(g, {l, 643, r, 646}, Rgb(0x1E232D));
        Fill(g, {l, 643, l + (r - l) * s.upd_progress, 646}, Rgb(0x52C8FF, 0.9f));
    } else {
        Text(g, L"Updates keep your game, saves and settings.", g_fonts.note.Get(), {l, 639, r, 657}, kFaint);
    }

    Text(g, std::wstring(L"TSFP RECOMP   /   ") + kVersion, g_fonts.note.Get(), {l, 670, r, 688}, Rgb(0x4A5060),
         DWRITE_TEXT_ALIGNMENT_LEADING, 1.5f);
    rt->SetTransform(was);
}

// ---- the settings, mods and saves pages ---------------------------------------------------------------------------

// settings.toml beside the game: its lines kept as they are, a top level `key = value` read or changed in place (the
// game writes it whole when it saves; what is changed here applies the next time it starts)
fs::path SettingsFile() { return ExeDir() / L"settings.toml"; }
std::vector<std::string> SettingsLines() {
    std::vector<std::string> lines;
    std::ifstream in(SettingsFile());
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}
std::string Trim(std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    return v;
}
// a top level key's value, its quotes taken off; def when it is not there
std::string Setting(const std::string& key, const std::string& def) {
    for (const std::string& line : SettingsLines()) {
        if (!line.empty() && line[0] == '[') break;
        size_t eq = line.find('=');
        if (eq == std::string::npos || Trim(line.substr(0, eq)) != key) continue;
        std::string v = Trim(line.substr(eq + 1));
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return def;
}
// a top level key set: a value as TOML writes it (a number, true or false, or a quoted string)
void SetSetting(const std::string& key, const std::string& toml) {
    std::vector<std::string> lines = SettingsLines();
    size_t at = lines.size();
    bool done = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].empty() && lines[i][0] == '[') {
            at = i;
            break;
        }
        size_t eq = lines[i].find('=');
        if (eq != std::string::npos && Trim(lines[i].substr(0, eq)) == key) {
            lines[i] = key + " = " + toml;
            done = true;
            break;
        }
    }
    if (!done) lines.insert(lines.begin() + at, key + " = " + toml);
    std::ofstream out(SettingsFile(), std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines) out << line << "\n";
}
std::string Quoted(const std::string& v) { return "'" + v + "'"; }
int SettingInt(const std::string& key, int def) {
    std::string v = Setting(key, "");
    return v.empty() ? def : std::atoi(v.c_str());
}
bool SettingBool(const std::string& key, bool def) {
    std::string v = Setting(key, "");
    return v.empty() ? def : v == "true";
}
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// a setting the page shows: its choices' names, which one is set, and setting one
struct Option {
    const wchar_t* label;
    std::vector<std::wstring> names;
    std::function<int()> get;
    std::function<void(int)> set;
};

// the graphics presets, as the game's own settings panel has them: render scale, MSAA, texture sharpness, edge
// smoothing, ambient occlusion
struct Preset {
    const wchar_t* name;
    int render, msaa, aniso, edge, ao;
};
constexpr Preset kPresets[] = {{L"Original", 100, 0, 0, 0, 0}, {L"Light", 100, 0, 4, 1, 0}, {L"Balanced", 100, 2, 8, 2, 1},
                               {L"High", 100, 4, 16, 2, 2},     {L"Ultra", 150, 4, 16, 2, 2}};

// a choice of strings saved as they are named in the file
Option Strings(const wchar_t* label, const char* key, std::vector<std::wstring> names, std::vector<std::string> values,
               const std::string& def) {
    return {label, std::move(names),
            [key, values, def] {
                std::string v = Setting(key, def);
                for (size_t i = 0; i < values.size(); i++)
                    if (values[i] == v) return int(i);
                return 0;
            },
            [key, values](int i) { SetSetting(key, Quoted(values[size_t(i)])); }};
}
Option Switch(const wchar_t* label, const char* key, bool def) {
    return {label, {L"Off", L"On"}, [key, def] { return SettingBool(key, def) ? 1 : 0; },
            [key](int i) { SetSetting(key, i ? "true" : "false"); }};
}
// a number among values, the nearest one shown as set
Option Numbers(const wchar_t* label, const char* key, std::vector<int> values, int def, const wchar_t* unit) {
    std::vector<std::wstring> names;
    for (int v : values) names.push_back(std::to_wstring(v) + unit);
    return {label, std::move(names),
            [key, values, def] {
                int v = SettingInt(key, def), best = 0;
                for (size_t i = 0; i < values.size(); i++)
                    if (std::abs(values[i] - v) < std::abs(values[size_t(best)] - v)) best = int(i);
                return best;
            },
            [key, values](int i) { SetSetting(key, std::to_string(values[size_t(i)])); }};
}

std::vector<Option> MakeOptions() {
    std::vector<Option> o;
    o.push_back(Strings(L"Display mode", "display_mode", {L"Fullscreen", L"Borderless", L"Windowed"},
                        {"fullscreen", "borderless", "windowed"}, "borderless"));
    // the monitor's modes, largest first
    std::vector<std::pair<int, int>> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) modes.push_back({int(dm.dmPelsWidth), int(dm.dmPelsHeight)});
    std::sort(modes.begin(), modes.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
    std::vector<std::wstring> names;
    for (auto [w, h] : modes) names.push_back(std::to_wstring(w) + L" x " + std::to_wstring(h));
    o.push_back({L"Resolution", names,
                 [modes] {
                     int w = SettingInt("width", 0), h = SettingInt("height", 0);
                     for (size_t i = 0; i < modes.size(); i++)
                         if (modes[i].first == w && modes[i].second == h) return int(i);
                     return 0;
                 },
                 [modes](int i) {
                     SetSetting("width", std::to_string(modes[size_t(i)].first));
                     SetSetting("height", std::to_string(modes[size_t(i)].second));
                 }});
    std::vector<std::wstring> presets;
    for (const Preset& p : kPresets) presets.push_back(p.name);
    presets.push_back(L"Custom");
    o.push_back({L"Graphics preset", presets,
                 [] {
                     for (size_t i = 0; i < std::size(kPresets); i++) {
                         const Preset& p = kPresets[i];
                         if (SettingInt("render_percent", 100) == p.render && SettingInt("msaa", 0) == p.msaa &&
                             SettingInt("anisotropic_filtering", 0) == p.aniso &&
                             std::min(SettingInt("edge_smoothing", 0), 2) == p.edge &&
                             std::min(SettingInt("ambient_occlusion", 0), 2) == p.ao)
                             return int(i);
                     }
                     return int(std::size(kPresets));
                 },
                 [](int i) {
                     if (i >= int(std::size(kPresets))) return;
                     const Preset& p = kPresets[i];
                     SetSetting("render_percent", std::to_string(p.render));
                     SetSetting("msaa", std::to_string(p.msaa));
                     SetSetting("anisotropic_filtering", std::to_string(p.aniso));
                     SetSetting("edge_smoothing", std::to_string(p.edge));
                     SetSetting("ambient_occlusion", std::to_string(p.ao));
                 }});
    o.push_back(Numbers(L"Render scale", "render_percent", {50, 67, 75, 85, 100, 125, 150, 175, 200}, 100, L"%"));
    o.push_back(Strings(L"Aspect ratio", "aspect_ratio", {L"Auto", L"4:3", L"16:9", L"16:10", L"21:9", L"32:9"},
                        {"auto", "4:3", "16:9", "16:10", "21:9", "32:9"}, "auto"));
    o.push_back(Numbers(L"Field of view", "fov", {40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100}, 55, L"\x00B0"));
    o.push_back(Switch(L"Vertical sync", "vsync", true));
    o.push_back(Switch(L"HDR", "hdr", false));
    o.push_back({L"HUD style", {L"Future Perfect", L"TimeSplitters 2", L"TimeSplitters 1"},
                 [] { return std::clamp(SettingInt("hud_style", 0), 0, 2); },
                 [](int i) { SetSetting("hud_style", std::to_string(i)); }});
    o.push_back(Strings(L"Language", "language", {L"English", L"Fran\x00E7" L"ais", L"Deutsch", L"Espa\x00F1ol", L"Italiano"},
                        {"english", "french", "german", "spanish", "italian"}, "english"));
    o.push_back(Switch(L"Skip intro videos", "skip_intros", false));
    o.push_back(Switch(L"Fast loading", "fast_loading", false));
    return o;
}

// a box with arrows either side of what is set; a click on either side steps through the choices
void DrawOption(Gfx& g, const Option& o, float y, float l, float r) {
    ID2D1RenderTarget* rt = g.rt.Get();
    int at = std::clamp(o.get(), 0, int(o.names.size()) - 1), n = int(o.names.size());
    Text(g, o.label, g_fonts.body.Get(), {l, y, r, y + 30}, kText, DWRITE_TEXT_ALIGNMENT_LEADING, 0,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F box = {r - 250, y + 2, r, y + 28};
    Fill(g, box, kBox);
    g.brush->SetColor(kLine);
    rt->DrawRectangle(box, g.brush.Get(), 1);
    Text(g, o.names[size_t(at)], g_fonts.body.Get(), box, kText, DWRITE_TEXT_ALIGNMENT_CENTER, 0,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int side = 0; side < 2; side++) {
        D2D1_RECT_F a = side ? D2D1_RECT_F{box.right - 30, box.top, box.right, box.bottom}
                             : D2D1_RECT_F{box.left, box.top, box.left + 30, box.bottom};
        int id = kButtons + int(g_ui.hot.size());
        bool hover = g_ui.hover == id;
        if (hover) Fill(g, a, Rgb(0x232A37));
        Text(g, side ? L"\x203A" : L"\x2039", g_fonts.play.Get(), a, hover ? kCyan : kDim, DWRITE_TEXT_ALIGNMENT_CENTER, 0,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto set = o.set;
        int next = (at + (side ? 1 : n - 1)) % n;
        g_ui.hot.push_back({a, [set, next] { set(next); }});
    }
    Fill(g, {l, y + 33, r, y + 34}, Rgb(0x1A1F29));
}

// a flat button the pages draw (their own click places)
void DrawFlat(Gfx& g, D2D1_RECT_F b, const wchar_t* text, std::function<void()> press) {
    int id = kButtons + int(g_ui.hot.size());
    bool hover = g_ui.hover == id;
    Fill(g, b, hover ? Rgb(0x232A37) : kButton);
    g.brush->SetColor(hover ? Rgb(0x52C8FF, 0.8f) : Rgb(0x343B49));
    g.rt->DrawRectangle(b, g.brush.Get(), 1);
    Text(g, text, g_fonts.button.Get(), b, kText, DWRITE_TEXT_ALIGNMENT_CENTER, 1, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_ui.hot.push_back({b, std::move(press)});
}

void DrawSettings(Gfx& g) {
    static std::vector<Option> options = MakeOptions();
    float l = kBanner + 42, r = kWidth - 42, y = 160;
    Text(g, L"Read by the game when it starts. Everything else is in its own settings panel: F1 in the game.",
         g_fonts.note.Get(), {l, y, r, y + 18}, kDim);
    y += 28;
    for (const Option& o : options) {
        DrawOption(g, o, y, l, r);
        y += 38;
    }
}

// mods/<folder> with its mod.toml (name, description); off when the setting mods_off lists the folder
struct ModInfo {
    std::string folder;
    std::wstring name, about;
};
std::vector<ModInfo> Mods() {
    std::vector<ModInfo> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ExeDir() / L"mods", ec)) {
        if (!e.is_directory()) continue;
        ModInfo m{e.path().filename().string(), e.path().filename().wstring(), L""};
        std::ifstream in(e.path() / L"mod.toml");
        for (std::string line; std::getline(in, line);) {
            size_t eq = line.find('='), open = line.find('"', eq + 1), close = line.rfind('"');
            if (eq == std::string::npos || open == std::string::npos || close <= open) continue;
            std::string key = Trim(line.substr(0, eq)), v = line.substr(open + 1, close - open - 1);
            if (key == "name") m.name = Wide(v);
            if (key == "description") m.about = Wide(v);
        }
        out.push_back(std::move(m));
    }
    return out;
}
bool ModOff(const std::string& folder) {
    std::string off = "," + Setting("mods_off", "") + ",";
    return off.find("," + folder + ",") != std::string::npos;
}
void SetModOn(const std::string& folder, bool on) {
    std::vector<std::string> list;
    std::string off = Setting("mods_off", "");
    for (size_t start = 0; start <= off.size();) {
        size_t comma = std::min(off.find(',', start), off.size());
        std::string f = off.substr(start, comma - start);
        if (!f.empty() && f != folder) list.push_back(f);
        start = comma + 1;
    }
    if (!on) list.push_back(folder);
    std::sort(list.begin(), list.end());
    std::string joined;
    for (const std::string& f : list) joined += (joined.empty() ? "" : ",") + f;
    SetSetting("mods_off", Quoted(joined));
}

void DrawMods(Gfx& g) {
    ID2D1RenderTarget* rt = g.rt.Get();
    float l = kBanner + 42, r = kWidth - 42, y = 160;
    Text(g, L"Each folder in mods replaces or adds game files. Switched here or in the game (F1, Mods).",
         g_fonts.note.Get(), {l, y, r, y + 18}, kDim);
    y += 30;
    std::vector<ModInfo> mods = Mods();
    if (mods.empty()) Text(g, L"No mods yet.", g_fonts.body.Get(), {l, y, r, y + 24}, kDim);
    for (const ModInfo& m : mods) {
        bool on = !ModOff(m.folder);
        Text(g, m.name, g_fonts.body.Get(), {l, y, r - 110, y + 22}, kText);
        rt->PushAxisAlignedClip({l, y + 23, r - 110, y + 58}, D2D1_ANTIALIAS_MODE_ALIASED);  // two lines at most
        Text(g, m.about, g_fonts.note.Get(), {l, y + 23, r - 110, y + 80}, kDim);
        rt->PopAxisAlignedClip();
        D2D1_RECT_F sw = {r - 86, y + 6, r, y + 34};
        int id = kButtons + int(g_ui.hot.size());
        bool hover = g_ui.hover == id;
        Fill(g, sw, on ? Rgb(0x1E5E8C) : kButton);
        g.brush->SetColor(hover ? kCyan : Rgb(0x343B49));
        rt->DrawRectangle(sw, g.brush.Get(), 1);
        Text(g, on ? L"ON" : L"OFF", g_fonts.button.Get(), sw, on ? Rgb(0xFFFFFF) : kDim, DWRITE_TEXT_ALIGNMENT_CENTER,
             1, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::string folder = m.folder;
        g_ui.hot.push_back({sw, [folder, on] { SetModOn(folder, !on); }});
        Fill(g, {l, y + 62, r, y + 63}, Rgb(0x1A1F29));
        y += 70;
        if (y > kHeight - 120) break;
    }
    DrawFlat(g, {l, kHeight - 70, l + 260, kHeight - 32}, L"OPEN MODS FOLDER", [] {
        std::error_code ec;
        fs::create_directories(ExeDir() / L"mods", ec);
        ShellExecuteW(g_window, L"open", (ExeDir() / L"mods").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

// the saves: hdd/E/UDATA/<title>/<save> with its SaveMeta.xbx (UTF-16 "Name=..." lines). A profile's folder has its
// tsftrpft, a MapMaker level's its .mkr
fs::path SavesFolder() { return ExeDir() / L"hdd" / L"E" / L"UDATA"; }
enum SaveKind { kProfiles, kLevels, kKinds };
struct SaveInfo {
    std::wstring name, when;
    fs::path folder;
};
std::vector<SaveInfo> Saves(SaveKind kind) {
    std::vector<SaveInfo> out;
    std::error_code ec;
    for (const auto& title : fs::directory_iterator(SavesFolder(), ec))
        for (const auto& save : fs::directory_iterator(title.path(), ec)) {
            std::vector<uint8_t> meta;
            if (!ReadFile(save.path() / L"SaveMeta.xbx", meta) || meta.size() < 4) continue;
            bool level = false;
            for (const auto& f : fs::directory_iterator(save.path(), ec))
                if (Lower(f.path().extension().wstring()) == L".mkr") level = true;
            if (level != (kind == kLevels)) continue;
            std::wstring text(reinterpret_cast<const wchar_t*>(meta.data()), meta.size() / 2);
            size_t at = text.find(L"Name=");
            std::wstring name = at == std::wstring::npos ? save.path().filename().wstring() : text.substr(at + 5);
            name = name.substr(0, name.find_first_of(L"\r\n"));
            if (level && name.rfind(L"MapMaker: ", 0) == 0) name = name.substr(10);
            auto t = fs::last_write_time(save.path() / L"SaveMeta.xbx", ec);
            auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
            std::time_t tt = std::chrono::system_clock::to_time_t(sys);
            std::tm tm{};
            localtime_s(&tm, &tt);
            wchar_t when[32];
            wcsftime(when, 32, L"%Y-%m-%d %H:%M", &tm);
            out.push_back({name, when, save.path()});
        }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.when > b.when; });
    return out;
}

std::wstring g_saves_note;
bool g_saves_error = false;
int g_saves_kind = kProfiles;
float g_saves_scroll = 0;  // rows scrolled past, by the mouse wheel

std::wstring Stamp() {
    std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &tt);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d_%H%M%S", &tm);
    return stamp;
}

// the saves copied whole to saves_backup/<date and time>
void BackUpSaves() {
    std::error_code ec;
    fs::path to = ExeDir() / L"saves_backup" / Stamp();
    fs::create_directories(to, ec);
    fs::copy(ExeDir() / L"hdd" / L"E", to / L"E", fs::copy_options::recursive, ec);
    g_saves_error = bool(ec);
    g_saves_note = ec ? L"The backup failed: " + Wide(ec.message()) : L"All saves copied to saves_backup\\" + to.filename().wstring();
}

// one save copied to saves_backup/exported/<its folder>, to share or keep, and that folder shown
void ExportSave(const SaveInfo& save) {
    std::error_code ec;
    fs::path to = ExeDir() / L"saves_backup" / L"exported" / save.folder.filename();
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    fs::copy(save.folder, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    g_saves_error = bool(ec);
    g_saves_note = ec ? L"The export failed: " + Wide(ec.message()) : L"\"" + save.name + L"\" exported to saves_backup\\exported";
    if (!ec) ShellExecuteW(g_window, L"open", to.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// one save to the Recycle Bin, after asking (it can be put back from there)
void DeleteSave(const SaveInfo& save) {
    std::wstring ask = L"Move \"" + save.name + L"\" to the Recycle Bin?\n\nIt can be restored from there.";
    if (MessageBoxW(g_window, ask.c_str(), L"Delete save", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    std::wstring from = save.folder.wstring();
    from.push_back(L'\0');  // a list of paths, ended by an empty one
    SHFILEOPSTRUCTW op{};
    op.hwnd = g_window;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    g_saves_error = !ok;
    g_saves_note = ok ? L"\"" + save.name + L"\" moved to the Recycle Bin." : L"It could not be deleted.";
}

// A save's data files (a profile's tsftrpft, a level's .mkr) end with a 20-byte HMAC-SHA1 of the rest. An Xbox or
// xemu keys it with the console's keys; the port's are all zero, so a save from there reads as damaged until its
// signature is worked out again with those. Returns the files re-signed, -1 when one could not be written
int Resign(const fs::path& save) {
    int count = 0;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(save, ec)) {
        std::wstring ext = Lower(f.path().extension().wstring());
        if (ext != L".mkr" && Lower(f.path().filename().wstring()) != L"tsftrpft") continue;
        std::vector<uint8_t> data;
        if (!ReadFile(f.path(), data) || data.size() <= 20) continue;
        uint8_t key[16] = {}, sig[20] = {};
        BCryptHash(BCRYPT_HMAC_SHA1_ALG_HANDLE, key, sizeof(key), data.data(), ULONG(data.size() - 20), sig, 20);
        if (!std::memcmp(sig, data.data() + data.size() - 20, 20)) continue;
        std::memcpy(data.data() + data.size() - 20, sig, 20);
        std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) return -1;
        count++;
    }
    return count;
}

// a save folder (with its SaveMeta.xbx, as exported, or from an Xbox or xemu), or a folder of them, copied in beside
// the others and re-signed for the port
void ImportSave() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS);
    dialog->SetTitle(L"A save's folder (the one with its SaveMeta.xbx), or a folder of saves");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(g_window)) || FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    fs::path from = path;
    CoTaskMemFree(path);
    std::error_code ec;
    std::vector<fs::path> saves;
    if (fs::exists(from / L"SaveMeta.xbx", ec)) saves.push_back(from);
    else
        for (const auto& d : fs::directory_iterator(from, ec))
            if (fs::exists(d.path() / L"SaveMeta.xbx", ec)) saves.push_back(d.path());
    if (saves.empty()) {
        g_saves_error = true;
        g_saves_note = L"No save there: a save's folder has its SaveMeta.xbx.";
        return;
    }
    // beside the game's other saves, under its title's folder
    fs::path title = SavesFolder() / L"45410066";
    for (const auto& t : fs::directory_iterator(SavesFolder(), ec)) title = t.path();
    int added = 0, there = 0, resigned = 0, failed = 0;
    for (const fs::path& save : saves) {
        fs::path to = title / save.filename();
        if (fs::exists(to, ec)) {
            there++;
            continue;
        }
        fs::create_directories(to, ec);
        fs::copy(save, to, fs::copy_options::recursive, ec);
        int n = ec ? -1 : Resign(to);
        if (n < 0) {
            failed++;
            fs::remove_all(to, ec);
            continue;
        }
        added++;
        resigned += n > 0;
    }
    g_saves_error = failed || !added;
    g_saves_note = std::to_wstring(added) + (added == 1 ? L" save imported" : L" saves imported");
    if (resigned) g_saves_note += L", " + std::to_wstring(resigned) + L" from an Xbox re-signed";
    if (there) g_saves_note += L", " + std::to_wstring(there) + L" already there (delete to replace)";
    if (failed) g_saves_note += L", " + std::to_wstring(failed) + L" could not be copied";
    g_saves_note += L".";
}

void DrawSaves(Gfx& g) {
    ID2D1RenderTarget* rt = g.rt.Get();
    float l = kBanner + 42, r = kWidth - 42, y = 158;
    // profiles or MapMaker levels
    std::vector<SaveInfo> lists[kKinds] = {Saves(kProfiles), Saves(kLevels)};
    const wchar_t* kinds[kKinds] = {L"PROFILES", L"MAPMAKER LEVELS"};
    float x = l;
    for (int k = 0; k < kKinds; k++) {
        std::wstring label = std::wstring(kinds[k]) + L"  " + std::to_wstring(lists[k].size());
        float w = k ? 190.f : 130.f;
        D2D1_RECT_F b = {x, y, x + w, y + 28};
        int id = kButtons + int(g_ui.hot.size());
        bool on = g_saves_kind == k, hover = g_ui.hover == id;
        Fill(g, b, on ? Rgb(0x1E5E8C) : hover ? Rgb(0x232A37) : kButton);
        g.brush->SetColor(on ? kCyan : Rgb(0x343B49));
        rt->DrawRectangle(b, g.brush.Get(), 1);
        Text(g, label, g_fonts.label.Get(), b, on ? Rgb(0xFFFFFF) : kText, DWRITE_TEXT_ALIGNMENT_CENTER, 1.5f,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_ui.hot.push_back({b, [k] { g_saves_kind = k, g_saves_scroll = 0; }});
        x += w + 10;
    }
    y += 40;

    // the rows, scrolled by the wheel
    const std::vector<SaveInfo>& saves = lists[g_saves_kind];
    const float row = 34, top = y, bottom = kHeight - 104;
    int fit = int((bottom - top) / row), most = std::max(0, int(saves.size()) - fit);
    g_saves_scroll = std::clamp(g_saves_scroll, 0.f, float(most));
    int first = int(g_saves_scroll);
    if (saves.empty())
        Text(g, g_saves_kind == kLevels ? L"No MapMaker levels saved yet." : L"No profiles yet.", g_fonts.body.Get(),
             {l, y, r, y + 24}, kDim);
    for (int i = first; i < int(saves.size()) && i < first + fit; i++) {
        const SaveInfo& sv = saves[size_t(i)];
        Text(g, sv.name, g_fonts.body.Get(), {l, y + 4, r - 300, y + 28}, kText);
        Text(g, sv.when, g_fonts.note.Get(), {r - 300, y + 8, r - 170, y + 28}, kDim);
        for (int a = 0; a < 2; a++) {
            D2D1_RECT_F b = {r - 160 + a * 82.f, y + 4, r - 82 + a * 82.f, y + 28};
            int id = kButtons + int(g_ui.hot.size());
            bool hover = g_ui.hover == id;
            Fill(g, b, hover ? (a ? Rgb(0x5A1E1A) : Rgb(0x232A37)) : kButton);
            g.brush->SetColor(hover ? (a ? kRed : kCyan) : Rgb(0x343B49));
            rt->DrawRectangle(b, g.brush.Get(), 1);
            Text(g, a ? L"DELETE" : L"EXPORT", g_fonts.label.Get(), b, a && hover ? kRed : kText,
                 DWRITE_TEXT_ALIGNMENT_CENTER, 1, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            SaveInfo copy = sv;
            g_ui.hot.push_back({b, [copy, a] { a ? DeleteSave(copy) : ExportSave(copy); }});
        }
        Fill(g, {l, y + row - 2, r, y + row - 1}, Rgb(0x1A1F29));
        y += row;
    }
    if (most > 0) {  // where the list is: a thin bar at the right
        float h = bottom - top, bar = std::max(24.f, h * float(fit) / float(saves.size()));
        float at = top + (h - bar) * (g_saves_scroll / float(most));
        Fill(g, {r + 10, top, r + 13, bottom}, Rgb(0x1A1F29));
        Fill(g, {r + 10, at, r + 13, at + bar}, Rgb(0x52C8FF, 0.6f));
    }

    if (!g_saves_note.empty())
        Text(g, g_saves_note, g_fonts.note.Get(), {l, kHeight - 96, r, kHeight - 78}, g_saves_error ? kRed : kCyan);
    float third = (r - l - 16) / 3;
    DrawFlat(g, {l, kHeight - 70, l + third, kHeight - 32}, L"BACK UP ALL", BackUpSaves);
    DrawFlat(g, {l + third + 8, kHeight - 70, l + 2 * third + 8, kHeight - 32}, L"IMPORT", ImportSave);
    DrawFlat(g, {r - third, kHeight - 70, r, kHeight - 32}, L"OPEN FOLDER", [] {
        std::error_code ec;
        fs::create_directories(SavesFolder(), ec);
        ShellExecuteW(g_window, L"open", SavesFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

// the right side: the title, the page names, the page shown
void DrawPage(Gfx& g, const Status& s) {
    ID2D1RenderTarget* rt = g.rt.Get();
    float l = kBanner + 42, r = kWidth - 42;
    Fill(g, {kBanner, kBar, kWidth, kHeight}, kBack);
    g_ui.hot.clear();

    Text(g, L"TIMESPLITTERS", g_fonts.heading.Get(), {l, 52, r, 96}, kText, DWRITE_TEXT_ALIGNMENT_LEADING, 1);
    Text(g, L"FUTURE PERFECT  \x00B7  RECOMPILED", g_fonts.label.Get(), {l + 2, 99, r, 118}, kCyan, DWRITE_TEXT_ALIGNMENT_LEADING, 3);
    if (g.icon) rt->DrawBitmap(g.icon.Get(), {r - 62, 50, r, 112}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    float x = l;
    for (int i = 0; i < kPages; i++) {
        float w = i == kPageSettings ? 92.f : 70.f;
        D2D1_RECT_F tab = {x, 122, x + w, 146};
        int id = kButtons + int(g_ui.hot.size());
        bool on = g_ui.page == i, hover = g_ui.hover == id;
        Text(g, kPageNames[i], g_fonts.label.Get(), tab, on ? kText : hover ? kCyan : kDim, DWRITE_TEXT_ALIGNMENT_LEADING,
             2, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (on) Fill(g, {tab.left, 144, tab.right - 18, 146}, kCyan);
        g_ui.hot.push_back({tab, [i] { g_ui.page = i; }});
        x += w + 8;
    }
    Fill(g, {l, 146, r, 147}, kLine);
    switch (g_ui.page) {
    case kPageSettings: DrawSettings(g); break;
    case kPageMods: DrawMods(g); break;
    case kPageSaves: DrawSaves(g); break;
    default: DrawPlay(g, s); break;
    }
}

void DrawBar(Gfx& g) {
    ID2D1RenderTarget* rt = g.rt.Get();
    Fill(g, {0, 0, kWidth, kBar}, kBarBack);
    if (g.icon) rt->DrawBitmap(g.icon.Get(), {12, 9, 30, 27}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    Text(g, L"TIMESPLITTERS: FUTURE PERFECT", g_fonts.bar.Get(), {40, 0, 600, kBar}, Rgb(0xB9BECA),
         DWRITE_TEXT_ALIGNMENT_LEADING, 0.6f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int id : {kMinimize, kClose}) {
        D2D1_RECT_F b = g_ui.buttons[id].rect;
        if (g_ui.hover == id) Fill(g, b, id == kClose ? Rgb(0xC42B1C) : Rgb(0x262B36));
        float cx = (b.left + b.right) / 2, cy = kBar / 2;
        g.brush->SetColor(kText);
        if (id == kMinimize) {
            rt->DrawLine({cx - 5, cy}, {cx + 5, cy}, g.brush.Get(), 1.2f);
        } else {
            rt->DrawLine({cx - 5, cy - 5}, {cx + 5, cy + 5}, g.brush.Get(), 1.2f);
            rt->DrawLine({cx - 5, cy + 5}, {cx + 5, cy - 5}, g.brush.Get(), 1.2f);
        }
    }
}

HRESULT Draw(Gfx& g) {
    Status s = GetStatus();
    g.rt->BeginDraw();
    g.rt->Clear(kBack);
    DrawBanner(g, g_ui.time);
    DrawPage(g, s);
    DrawBar(g);
    return g.rt->EndDraw();
}

void CreateFonts() {
    g_fonts.heading = Font(L"Palatino Linotype", 36, DWRITE_FONT_WEIGHT_NORMAL);
    g_fonts.title = Font(L"Palatino Linotype", 22, DWRITE_FONT_WEIGHT_NORMAL);
    g_fonts.tagline = Font(L"Palatino Linotype", 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    g_fonts.body = Font(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_NORMAL);
    g_fonts.note = Font(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL);
    g_fonts.label = Font(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fonts.bar = Font(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL);
    g_fonts.button = Font(L"Bahnschrift", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fonts.play = Font(L"Bahnschrift", 19, DWRITE_FONT_WEIGHT_BOLD);
}

// ---- the window ------------------------------------------------------------------------------------------------

Gfx g_gfx;
float g_scale = 1;  // pixels per DIP

void Resize() {
    RECT rc;
    GetClientRect(g_window, &rc);
    if (g_gfx.rt) {
        ComPtr<ID2D1HwndRenderTarget> hwnd;
        g_gfx.rt.As(&hwnd);
        hwnd->Resize(D2D1::SizeU(rc.right, rc.bottom));
        g_gfx.rt->SetDpi(96 * g_scale, 96 * g_scale);
    }
}

void CreateTarget() {
    RECT rc;
    GetClientRect(g_window, &rc);
    ComPtr<ID2D1HwndRenderTarget> hwnd;
    g_d2d->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * g_scale, 96 * g_scale),
        D2D1::HwndRenderTargetProperties(g_window, D2D1::SizeU(rc.right, rc.bottom)), &hwnd);
    g_gfx = {};
    g_gfx.rt = hwnd;
    CreateResources(g_gfx);
}

void Launch(const std::wstring& args) {
    // checked again just before: the files can change while the launcher is up
    Check c = Scan();
    if (c.found != Found::Ready) {
        Update([&](Status& s) { s.check = c, s.note = L"The game is no longer ready: see above.", s.error = true; });
        return;
    }
    fs::path exe = ExeDir() / kGameExe;
    std::wstring cmd = L"\"" + exe.wstring() + L"\"" + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
        Update([](Status& s) { s.note = L"Could not start TSFP.exe: it must be beside the launcher.", s.error = true; });
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g_window);
}

void Browse() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    COMDLG_FILTERSPEC types[] = {{L"The game's disc image or default.xbe", L"*.iso;default.xbe"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, types);
    dialog->SetTitle(L"Your copy of TimeSplitters Future Perfect (Xbox, US)");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->Show(g_window)) && SUCCEEDED(dialog->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        StartImport(path);
        CoTaskMemFree(path);
    }
}

// ---- updates ---------------------------------------------------------------------------------------------------

// a text file's line (0 the first), trimmed
std::wstring ReadLine(const fs::path& file, int which = 0) {
    std::wstring line;
    if (FILE* f = _wfopen(file.c_str(), L"rb")) {
        char text[512] = {};
        size_t n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        size_t i = 0;
        for (int skip = 0; skip < which && i < n; skip++) {
            while (i < n && text[i] != '\n') i++;
            i++;
        }
        for (; i < n && text[i] != '\r' && text[i] != '\n'; i++) line += wchar_t(text[i]);
        line.erase(0, line.find_first_not_of(L" \t"));
        line.erase(line.find_last_not_of(L" \t") + 1);
    }
    return line;
}

std::wstring g_repo, g_token;  // owner/name and, for a private repository, a read-only token: update.txt's two lines

// an HTTPS GET into out (or a file, with progress), false on a failure. GitHub's own addresses get the token;
// redirects are followed here, without it (the files' storage refuses a second credential)
bool Get(std::string url, std::string* out, const fs::path* file = nullptr, bool binary = false) {
  for (int hop = 0; hop < 5; hop++) {
    std::wstring wurl = Wide(url);
    URL_COMPONENTS u{sizeof(u)};
    wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host, u.dwHostNameLength = DWORD(std::size(host));
    u.lpszUrlPath = path, u.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[2048] = {};
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) return false;
    std::wstring target = std::wstring(path) + extra;
    bool ok = false;
    std::string next;
    HINTERNET session = WinHttpOpen(L"TSFP-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, u.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, nullptr, nullptr,
                                                     u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    }
    std::wstring headers = binary ? L"Accept: application/octet-stream\r\n" : L"Accept: application/vnd.github+json\r\n";
    if (!g_token.empty() && std::wstring(host).ends_with(L"github.com")) headers += L"Authorization: Bearer " + g_token + L"\r\n";
    if (request && WinHttpSendRequest(request, headers.c_str(), DWORD(-1), nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
        if (code >= 300 && code < 400) {
            wchar_t location[4096] = {};
            DWORD lsize = sizeof location;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, nullptr, location, &lsize, nullptr))
                for (wchar_t* c = location; *c; c++) next += char(*c);
        }
        uint64_t total = 0;
        wchar_t length[32] = {};
        DWORD lsize = sizeof length;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, length, &lsize, nullptr))
            total = _wcstoui64(length, nullptr, 10);
        FILE* f = file ? _wfopen(file->c_str(), L"wb") : nullptr;
        if (code == 200 && (!file || f)) {
            ok = true;
            uint64_t done = 0;
            std::vector<char> buffer(1 << 16);
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &got)) {
                    ok = false;
                    break;
                }
                if (!got) break;
                if (f) fwrite(buffer.data(), 1, got, f);
                else out->append(buffer.data(), got);
                done += got;
                if (f && total) Update([&](Status& s) { s.upd_progress = float(double(done) / double(total)); });
            }
            if (total && done != total) ok = false;
        }
        if (f) fclose(f);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    if (next.empty()) return ok;
    url = next;
  }
  return false;
}

// a JSON string's value after key, from at
std::string JsonString(const std::string& json, const std::string& key, size_t at = 0, size_t* end = nullptr) {
    size_t k = json.find("\"" + key + "\"", at);
    if (k == std::string::npos) return {};
    size_t q = json.find('"', json.find(':', k) + 1);
    if (q == std::string::npos) return {};
    size_t e = json.find('"', q + 1);
    if (end) *end = e;
    return e == std::string::npos ? std::string() : json.substr(q + 1, e - q - 1);
}

void CheckUpdate() {
    Update([](Status& s) { s.upd = Upd::Checking; });
    std::string json;
    if (!Get("https://api.github.com/repos/" + std::string(g_repo.begin(), g_repo.end()) + "/releases/latest", &json)) {
        Update([](Status& s) { s.upd = Upd::Failed, s.upd_note = L"Could not reach the update server."; });
        return;
    }
    std::string tag = JsonString(json, "tag_name"), url;
    for (size_t at = json.find("\"assets\""); at != std::string::npos;) {
        size_t end = 0;
        size_t key = json.find("\"name\"", at);
        std::string name = JsonString(json, "name", at, &end);
        if (name.empty()) break;
        if (name == "TSFP.zip") {
            // the asset's API address (its "url", before its name): it serves a private repository's file too
            size_t api = json.rfind("\"url\"", key);
            url = api != std::string::npos && api > at ? JsonString(json, "url", api) : JsonString(json, "browser_download_url", end);
            break;
        }
        at = json.find("browser_download_url", end);
        if (at != std::string::npos) at += 20;
    }
    Update([&](Status& s) {
        s.latest = Wide(tag), s.url = url;
        if (tag.empty() || url.empty()) s.upd = Upd::Failed, s.upd_note = L"The latest release has no TSFP.zip.";
        else s.upd = s.latest == s.version ? Upd::Current : Upd::Available;
    });
}

// runs a program and waits for it, its exit code (-1 when it could not start)
int RunWait(const std::wstring& cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

void Fail(const std::wstring& why) { Update([&](Status& s) { s.upd = Upd::Failed, s.upd_note = why, s.upd_progress = -1; }); }

// one file of the update over the installed one: a file in use (a DLL the launcher loaded) is moved aside to .old
// first, and a fresh one the antivirus is still reading is waited for; the Windows error when it cannot
DWORD Install(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    // one already the same is left as it is
    std::vector<uint8_t> a, b;
    if (fs::file_size(from, ec) == fs::file_size(to, ec) && !ec && ReadFile(from, a) && ReadFile(to, b) && a == b) return 0;
    fs::create_directories(to.parent_path(), ec);
    DWORD err = 0;
    for (int i = 0; i < 40; i++) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return 0;
        err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION) {
            std::wstring old = to.wstring() + L".old";
            DeleteFileW(old.c_str());
            if (fs::exists(to, ec) && MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) &&
                CopyFileW(from.c_str(), to.c_str(), FALSE))
                return 0;
        }
        Sleep(250);
    }
    return err;
}

// the latest TSFP.zip over this install: downloaded and unpacked into update\, then copied over the files here
// (the running launcher moved aside first); the game in assets, saves and settings are not in the zip
void ApplyUpdate() {
    Status s = GetStatus();
    fs::path dir = ExeDir(), work = dir / "update", zip = work / "TSFP.zip", files = work / "files";
    // the game must be closed: its executable is replaced
    HANDLE game = CreateFileW((dir / kGameExe).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (game == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) return Fail(L"Close the game first.");
    if (game != INVALID_HANDLE_VALUE) CloseHandle(game);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(files, ec);
    Update([](Status& st) { st.upd = Upd::Working, st.upd_note = L"Downloading\x2026", st.upd_progress = 0; });
    if (!Get(s.url, nullptr, &zip, true)) return Fail(L"The download failed. Try again.");
    Update([](Status& st) { st.upd_note = L"Unpacking\x2026", st.upd_progress = -1; });
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    if (RunWait(L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" + files.wstring() + L"\"") != 0)
        return Fail(L"The download could not be unpacked.");
    if (!fs::exists(files / kGameExe)) return Fail(L"The download is not a game release.");
    Update([](Status& st) { st.upd_note = L"Installing\x2026"; });
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path launcher = self, old = launcher.wstring() + L".old";
    fs::remove(old, ec);
    if (!MoveFileExW(launcher.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return Fail(L"Could not replace the launcher.");
    for (auto it = fs::recursive_directory_iterator(files, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = it->path().lexically_relative(files);
        if (DWORD err = Install(it->path(), dir / rel)) {
            if (!fs::exists(launcher, ec)) MoveFileExW(old.c_str(), launcher.c_str(), MOVEFILE_REPLACE_EXISTING);
            return Fail(L"Could not copy " + rel.wstring() + L" (error " + std::to_wstring(err) + L").");
        }
    }
    fs::remove_all(work, ec);
    // the new launcher, this one gone
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + launcher.wstring() + L"\" --updated";
    if (CreateProcessW(launcher.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    PostMessageW(g_window, WM_CLOSE, 0, 0);
}

// the installed version and the update source, an old launcher left by an update removed, a first check
void StartUpdates() {
    fs::path dir = ExeDir();
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::error_code ec;
    fs::remove(std::wstring(self) + L".old", ec);
    for (auto& e : fs::directory_iterator(dir, ec))
        if (Lower(e.path().extension().wstring()) == L".old") fs::remove(e.path(), ec);
    fs::remove_all(dir / "update", ec);
    g_repo = ReadLine(dir / "update.txt");
    g_token = ReadLine(dir / "update.txt", 1);
    std::wstring version = ReadLine(dir / "version.txt");
    Update([&](Status& s) { s.version = version, s.upd = g_repo.empty() ? Upd::Off : Upd::Checking; });
    if (!g_repo.empty()) std::thread(CheckUpdate).detach();
}

void Press(int id) {
    if (id >= kButtons) {
        if (size_t(id - kButtons) < g_ui.hot.size()) {
            std::function<void()> press = g_ui.hot[size_t(id - kButtons)].press;
            press();
        }
        return;
    }
    Status s = GetStatus();
    if (!Enabled(id, s)) return;
    switch (id) {
    case kImport:
        if (s.busy) g_cancel = true;
        else Browse();
        break;
    case kPlay: Launch(L""); break;
    case kVerify:
        Update([](Status& st) { st.busy = true, st.check.found = Found::Checking, st.note.clear(); });
        std::thread([] {
            Check c = Scan();
            Rescan(c.found == Found::Ready ? L"Verified: the game is whole and the right release." : L"", c.found != Found::Ready);
        }).detach();
        break;
    case kFolder: {
        std::error_code ec;
        fs::create_directories(Assets(), ec);
        ShellExecuteW(g_window, L"open", Assets().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kUpdate:
        if (s.upd == Upd::Available) std::thread(ApplyUpdate).detach();
        else std::thread(CheckUpdate).detach();
        break;
    case kMinimize: ShowWindow(g_window, SW_MINIMIZE); break;
    case kClose: DestroyWindow(g_window); break;
    }
}

int HitButton(LPARAM lp) {
    float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
    for (int id = g_ui.page == kPagePlay ? 0 : kMinimize; id < kButtons; id++) {
        const D2D1_RECT_F& r = g_ui.buttons[id].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return id;
    }
    for (size_t i = 0; i < g_ui.hot.size(); i++) {
        const D2D1_RECT_F& r = g_ui.hot[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return kButtons + int(i);
    }
    return -1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float y = pt.y / g_scale, x = pt.x / g_scale;
        if (y < kBar && x < kWidth - 92) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        int hit = HitButton(lp);
        if (hit != g_ui.hover) {
            g_ui.hover = hit;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_ui.hover = -1; return 0;
    case WM_MOUSEWHEEL:
        if (g_ui.page == kPageSaves) g_saves_scroll -= float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 2;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_ui.hover >= 0 &&
            ((g_ui.hover < kMinimize && Enabled(g_ui.hover, GetStatus())) || g_ui.hover >= kButtons)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        g_ui.pressed = HitButton(lp);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int hit = HitButton(lp), pressed = g_ui.pressed;
        g_ui.pressed = -1;
        if (hit >= 0 && hit == pressed) Press(hit);
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(HDROP(wp), 0, path, DWORD(std::size(path)))) StartImport(path);
        DragFinish(HDROP(wp));
        return 0;
    }
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.f;
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Resize();
        return 0;
    }
    case WM_SIZE: Resize(); return 0;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// one frame drawn into a picture, for looking at the design without the window
int Snapshot(const std::wstring& file, const std::wstring& state, double time) {
    const float scale = 2;
    ComPtr<IWICBitmap> bitmap;
    g_wic->CreateBitmap(UINT(kWidth * scale), UINT(kHeight * scale), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    g_d2d->CreateWicBitmapRenderTarget(
        bitmap.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale),
        &g_gfx.rt);
    CreateResources(g_gfx);
    Status& s = g_status;
    for (int i = 0; i < kPages; i++)
        if (state == Lower(kPageNames[i])) g_ui.page = i;
    if (state == L"levels") g_ui.page = kPageSaves, g_saves_kind = kLevels;
    if (state == L"ready" || g_ui.page != kPagePlay) s.check = {Found::Ready, L"TimeSplitters - Future Perfect (USA).iso", {}};
    else if (state == L"importing") s.busy = true, s.progress = 0.42f;
    else if (state == L"glitch") s.check = {Found::Ready, L"TimeSplitters - Future Perfect (USA).iso", {}};
    else s.check = {Found::Missing};
    s.version = L"2026.10.29.1415", s.upd = Upd::Available, s.latest = L"2026.10.30.0900";
    g_ui.hover = state == L"ready" ? kPlay : -1;
    for (int i = 0; i < 240; i++) Simulate(1 / 60.f);
    g_ui.time = time >= 0 ? time : state == L"glitch" ? 6.85 : 2.2;
    Draw(g_gfx);

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    g_wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) return 1;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->WriteSource(bitmap.Get(), nullptr);
    frame->Commit();
    encoder->Commit();
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    CreateFonts();
    Layout();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && std::wstring(argv[1]) == L"--snapshot") {
        g_snapshot = true;
        return Snapshot(argv[2], argc >= 4 ? argv[3] : L"missing", argc >= 5 ? _wtof(argv[4]) : -1);
    }

    // one launcher at a time; after an update the old one is still closing
    bool updated = argc >= 2 && std::wstring(argv[1]) == L"--updated";
    HANDLE once = CreateMutexW(nullptr, TRUE, L"TSFP Launcher");
    for (int i = 0; updated && GetLastError() == ERROR_ALREADY_EXISTS && i < 50; i++) {
        CloseHandle(once);
        Sleep(200);
        once = CreateMutexW(nullptr, TRUE, L"TSFP Launcher");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"TSFPLauncher", nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TSFPLauncher";
    RegisterClassExW(&wc);

    // centered on the screen the cursor is on, sized for its scale
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96;
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        UINT dx = 96, dy = 96;
        if (auto f = GetDpi(GetProcAddress(shcore, "GetDpiForMonitor")); f && SUCCEEDED(f(monitor, 0, &dx, &dy))) dpi = dx;
    }
    g_scale = dpi / 96.f;
    int w = int(kWidth * g_scale), h = int(kHeight * g_scale);
    RECT work = mi.rcWork;
    g_window = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"TimeSplitters Future Perfect",
                               WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                               work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                               nullptr, nullptr, inst, nullptr);
    DWORD round = 2;  // DWMWCP_ROUND, on Windows 11
    DwmSetWindowAttribute(g_window, 33, &round, sizeof(round));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark));

    CreateTarget();
    DragAcceptFiles(g_window, TRUE);
    ShowWindow(g_window, show);
    std::thread([] { Rescan(); }).detach();
    StartUpdates();

    // drawn each frame the screen shows, the portal moving
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return int(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        float dt = std::min(0.1f, float(double(now.QuadPart - last.QuadPart) / freq.QuadPart));
        last = now;
        if (IsIconic(g_window)) {
            WaitMessage();
            continue;
        }
        g_ui.time += dt;
        Simulate(dt);
        if (Draw(g_gfx) == D2DERR_RECREATE_TARGET) CreateTarget();
        DwmFlush();  // paced by the compositor
    }
}
