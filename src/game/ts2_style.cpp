// The look of the early Future Perfect, which was TimeSplitters 2's: its font, read from the player's own copy of
// TimeSplitters 2 for Xbox (its default.xbe and data paks, never shipped with this port), or the Arial Bold the early
// subtitles were in, which this game still has.
//
// The game's fonts (0x4C8C28, 0x38 bytes each): {texture 0x3C4 "impact" / 0x3C5 "arialbold", ?, glyphs, accents,
// per glyph spacing, kerning pairs, line height, ...}. Its text styles (0x4C8DE8, 30 of 0x14 bytes: {font, ?, scale x,
// scale y, spacing}) use font 0 for 0 to 13 and font 1 for 14 to 29. A glyph (impact's at 0x4C9FA0, 12 bytes) is
// {x, y, width, height, float y offset} on the 256x256 texture, for the character less 0x20 (0x3FAA0 measures them).
// TimeSplitters 2's font record is {"host0:textures/misc/impact.raw", ?, glyphs, accents, ...}, its glyphs 10 bytes
// {x0, y0, x1, y1, y offset}: the same characters in the same order up to 'ß', with no spacing or kerning tables.
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace game {
fs::path Ts2Root();
}

namespace {

constexpr uint32_t kImpact = 0x4C8C28;                    // font 0
constexpr uint32_t kSpacingTable = 0x10, kKerning = 0x14;  // in a font
constexpr uint32_t kGlyphs = 0x4C9FA0, kGlyphSize = 12, kGlyphCount = 106;
constexpr uint32_t kStyles = 0x4C8DE8, kStyleSize = 0x14, kStyleCount = 30, kImpactStyles = 14;
constexpr uint32_t kStyleSpacing = 0x10;
constexpr uint32_t kTs2Glyphs = 101;  // ' ' to 'ß', alike in both
// Future Perfect's own after those (æ œ Æ Ç ®) as TimeSplitters 2's nearest (a o A Ç ©)
constexpr uint32_t kTs2Nearest[] = {65, 79, 33, 99, 96};
// TimeSplitters 2's glyphs have a pixel each side, Impact's about two: its styles' spacing two more
constexpr int32_t kTs2Spacing = 2;
constexpr char kFontTexture[] = "assets/ts/textures/xbox/impact.xbt";
constexpr char kTs2FontName[] = "host0:textures/misc/impact.raw";
constexpr char kTs2FontFile[] = "textures/misc/impact.xbt";
constexpr char kPackedGlyphs[] = "glyphs.bin";  // in the ts2 folder beside the game, in place of default.xbe
// a texture file: a header of 0x80 bytes {width, height, width, height, mip levels less one, format, ...}, then the
// pixels. Impact's is 256x256 8 bit alpha (format 5) in the Xbox's swizzled order, TimeSplitters 2's DXT3 (format 1)
// with its mip levels
constexpr uint32_t kHeader = 0x80, kSide = 256, kAlpha8 = 5, kDxt3 = 1;

// TimeSplitters 2's font as Impact's file, put over its bytes where the pak is read: the game reads its textures
// with the pak's own reads, many files at once, not through its file calls
std::vector<uint8_t> g_font;
uint64_t g_font_at = 0;  // where Impact's file is in its pak (arcade/l_103.pak)

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

uint32_t Get32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, &d[at], 4);
    return v;
}

// the pixel's place in a square texture's swizzled order: x and y's bits interleaved
uint32_t Swizzle(uint32_t x, uint32_t y) {
    uint32_t o = 0;
    for (uint32_t b = 0; b < 8; b++) o |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return o;
}

// TimeSplitters 2's texture (DXT3) as Impact's (8 bit alpha, swizzled), Impact's header kept
bool ConvertFont(const std::vector<uint8_t>& ts2, const std::vector<uint8_t>& impact, std::vector<uint8_t>& out) {
    auto field = [](const std::vector<uint8_t>& d, int i) { return Get32(d, size_t(i) * 4); };
    if (field(ts2, 0) != kSide || field(ts2, 1) != kSide || field(ts2, 5) != kDxt3 ||
        ts2.size() < kHeader + kSide * kSide)
        return false;
    if (field(impact, 0) != kSide || field(impact, 1) != kSide || field(impact, 5) != kAlpha8 ||
        impact.size() != kHeader + kSide * kSide)
        return false;
    out.assign(impact.begin(), impact.begin() + kHeader);
    out.resize(impact.size());
    // a DXT3 block of 4x4 is 16 bytes: the alphas, 4 bits each a row at a time, then the colours
    for (uint32_t by = 0; by < kSide / 4; by++)
        for (uint32_t bx = 0; bx < kSide / 4; bx++) {
            const uint8_t* block = &ts2[kHeader + (by * (kSide / 4) + bx) * 16];
            for (uint32_t p = 0; p < 16; p++) {
                uint8_t a = (block[p / 2] >> (p % 2 * 4)) & 0xF;
                out[kHeader + Swizzle(bx * 4 + p % 4, by * 4 + p / 4)] = uint8_t(a * 17);
            }
        }
    return true;
}

// a path's name in the game's paks
uint32_t Crc32(const char* s) {
    uint32_t c = ~0u;
    for (; *s; s++) {
        c ^= uint8_t(*s);
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// files in TimeSplitters 2's paks (data/**.pak, "P8CK"): {magic, directory, count, names}, the directory 12 bytes a
// file: {name offset in names, size, offset}. Each from the first pak with it
std::map<std::string, std::vector<uint8_t>> Ts2PakFiles(const fs::path& data, const std::set<std::string>& wanted) {
    std::map<std::string, std::vector<uint8_t>> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(data, ec);
         !ec && it != fs::recursive_directory_iterator() && out.size() < wanted.size(); it.increment(ec)) {
        if (it->path().extension() != ".pak") continue;
        std::ifstream f(it->path(), std::ios::binary);
        uint32_t head[4] = {};
        if (!f.read(reinterpret_cast<char*>(head), 16) || std::memcmp(head, "P8CK", 4)) continue;
        uint32_t dir = head[1], count = head[2], names = head[3];
        if (names >= dir || count > 100000) continue;
        std::vector<char> table(dir - names + size_t(count) * 12);
        if (!f.seekg(names) || !f.read(table.data(), std::streamsize(table.size()))) continue;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t e[3];
            std::memcpy(e, &table[dir - names + size_t(i) * 12], 12);
            if (e[0] >= dir - names) continue;
            std::string name(&table[e[0]], strnlen(&table[e[0]], dir - names - e[0]));
            if (!wanted.count(name) || out.count(name)) continue;
            std::vector<uint8_t> bytes(e[1]);
            if (f.seekg(e[2]) && f.read(reinterpret_cast<char*>(bytes.data()), e[1])) out[name] = std::move(bytes);
        }
    }
    return out;
}

bool Ts2PakFile(const fs::path& data, const char* name, std::vector<uint8_t>& out) {
    auto files = Ts2PakFiles(data, {name});
    if (files.empty()) return false;
    out = std::move(files.begin()->second);
    return true;
}

// TimeSplitters 2's glyphs, {x0, y0, x1, y1, y offset}, from its default.xbe: its font record found by the name
std::vector<std::array<uint16_t, 5>> Ts2Glyphs(const fs::path& xbe) {
    std::vector<uint8_t> d;
    if (!ReadFile(xbe, d) || d.size() < 0x200 || std::memcmp(d.data(), "XBEH", 4)) return {};
    uint32_t base = Get32(d, 0x104), count = Get32(d, 0x11C), headers = Get32(d, 0x120) - base;
    struct Section {
        uint32_t va, raw, size;
    };
    std::vector<Section> sections;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        size_t h = headers + size_t(i) * 56;
        sections.push_back({Get32(d, h + 4), Get32(d, h + 12), Get32(d, h + 16)});
    }
    auto offset = [&](uint32_t va) -> size_t {
        for (auto& s : sections)
            if (va >= s.va && va - s.va < s.size && size_t(s.raw) + s.size <= d.size()) return s.raw + (va - s.va);
        return 0;
    };
    auto address = [&](size_t off) -> uint32_t {
        for (auto& s : sections)
            if (off >= s.raw && off - s.raw < s.size) return s.va + uint32_t(off - s.raw);
        return 0;
    };
    auto at = std::search(d.begin(), d.end(), kTs2FontName, kTs2FontName + sizeof kTs2FontName);
    uint32_t name = at == d.end() ? 0 : address(size_t(at - d.begin()));
    if (!name) return {};
    for (size_t i = 0; i + 12 <= d.size(); i += 4) {
        if (Get32(d, i) != name) continue;
        size_t glyphs = offset(Get32(d, i + 8));
        if (!glyphs || glyphs + kTs2Glyphs * 10 > d.size()) continue;
        std::vector<std::array<uint16_t, 5>> out(kTs2Glyphs);
        bool fits = true;
        for (uint32_t g = 0; g < kTs2Glyphs; g++) {
            std::memcpy(out[g].data(), &d[glyphs + g * 10], 10);
            auto& r = out[g];
            fits &= r[0] <= r[2] && r[2] <= 256 && r[1] <= r[3] && r[3] <= 256;
        }
        if (fits) return out;
    }
    return {};
}

// the glyphs as tools/ts2_pack.py writes them for the ts2 folder: kTs2Glyphs records of 10 bytes
std::vector<std::array<uint16_t, 5>> PackedGlyphs(const fs::path& file) {
    std::vector<uint8_t> d;
    if (!ReadFile(file, d) || d.size() != kTs2Glyphs * 10) return {};
    std::vector<std::array<uint16_t, 5>> out(kTs2Glyphs);
    for (uint32_t g = 0; g < kTs2Glyphs; g++) std::memcpy(out[g].data(), &d[g * 10], 10);
    return out;
}

std::vector<std::array<uint16_t, 5>> g_ts2_glyphs;  // TimeSplitters 2's, read when first used
std::vector<uint8_t> g_ts2_texture;
std::vector<uint8_t> g_impact;  // Impact's file as its pak has it
std::string g_pak;              // the font's pak as the game names it, once the game has read it
uint32_t g_mode = 0;            // the font the tables and the texture hold: Opt().font's values
std::recursive_mutex g_lock;
// the game's own tables, to go back to
std::vector<uint8_t> g_glyphs0, g_styles0;
uint32_t g_spacing0 = 0, g_kerning0 = 0;

bool LoadTs2() {
    if (!g_ts2_glyphs.empty() && !g_ts2_texture.empty()) return true;
    fs::path root = game::Ts2Root();
    if (root.empty()) {
        LOGR("font: TimeSplitters 2's needs its folder (General > TimeSplitters 2 folder)");
        return false;
    }
    g_ts2_glyphs = Ts2Glyphs(root / "default.xbe");
    if (g_ts2_glyphs.empty()) g_ts2_glyphs = PackedGlyphs(root / kPackedGlyphs);
    if (g_ts2_glyphs.empty()) {
        LOGR("font: no TimeSplitters 2 font record in %s", root.u8string().c_str());
        return false;
    }
    if (!Ts2PakFile(root / "data", kTs2FontFile, g_ts2_texture)) {
        LOGR("font: no %s in %s", kTs2FontFile, (root / "data").u8string().c_str());
        return false;
    }
    return true;
}

// Impact's file in its pak: {"P5CK", directory offset, directory size}, the directory 16 bytes a file: {CRC-32 of the
// path, offset, size, 0}. The paks' drive is the game's own, mounted after the image loads.
bool LoadImpact() {
    if (!g_impact.empty()) return true;
    std::vector<uint8_t> pak;
    if (g_pak.empty() || !kernel::ReadGameFile(g_pak, pak) || pak.size() < 16 || std::memcmp(pak.data(), "P5CK", 4))
        return false;
    uint32_t dir = Get32(pak, 4), dir_size = Get32(pak, 8), hash = Crc32(kFontTexture);
    for (uint64_t e = dir; e + 16 <= uint64_t(dir) + dir_size && e + 16 <= pak.size(); e += 16) {
        uint32_t off = Get32(pak, e + 4), size = Get32(pak, e + 8);
        if (Get32(pak, e) != hash || uint64_t(off) + size > pak.size()) continue;
        g_impact.assign(pak.begin() + off, pak.begin() + off + size);
        g_font_at = off;
        return true;
    }
    LOGR("font: no %s in %s", kFontTexture, g_pak.c_str());
    return false;
}

// TimeSplitters 2's font as Impact's file
bool Ts2Ready() {
    if (!g_font.empty()) return true;
    if (!LoadTs2() || !LoadImpact()) return false;
    if (!ConvertFont(g_ts2_texture, g_impact, g_font)) {
        LOGR("font: Impact's texture (%zu bytes) or TimeSplitters 2's (%zu) not as expected", g_impact.size(),
             g_ts2_texture.size());
        g_font.clear();
        return false;
    }
    return true;
}

// the glyphs and styles for a font: the game's, TimeSplitters 2's glyphs, or Impact's styles on Arial Bold
void SetTables(uint32_t mode) {
    std::memcpy(kernel::HostPtr(kGlyphs), g_glyphs0.data(), g_glyphs0.size());
    std::memcpy(kernel::HostPtr(kStyles), g_styles0.data(), g_styles0.size());
    kernel::W32(kImpact + kSpacingTable, g_spacing0);
    kernel::W32(kImpact + kKerning, g_kerning0);
    if (mode == 2)
        for (uint32_t s = 0; s < kImpactStyles; s++) kernel::W32(kStyles + s * kStyleSize, 1);
    if (mode != 1) return;
    for (uint32_t i = 0; i < kGlyphCount; i++) {
        auto& r = g_ts2_glyphs[i < kTs2Glyphs ? i : kTs2Nearest[i - kTs2Glyphs]];
        uint32_t g = kGlyphs + i * kGlyphSize;
        kernel::W16(g, r[0]);
        kernel::W16(g + 2, r[1]);
        kernel::W16(g + 4, uint16_t(r[2] - r[0]));
        kernel::W16(g + 6, uint16_t(r[3] - r[1]));
        float y = float(r[4]);
        std::memcpy(kernel::HostPtr(g + 8), &y, 4);
    }
    // Impact's spacing and kerning tables do not fit these glyphs
    kernel::W32(kImpact + kSpacingTable, 0);
    kernel::W32(kImpact + kKerning, 0);
    for (uint32_t s = 0; s < kImpactStyles; s++) {
        uint32_t at = kStyles + s * kStyleSize + kStyleSpacing;
        kernel::W32(at, uint32_t(int32_t(kernel::R32(at)) + kTs2Spacing));
    }
}

// the loaded font texture's pixels (and the load's copies) changed from one file's to the other's: found in
// contiguous memory, 128 byte aligned as textures are; the renderer sees the bytes change and uploads them again
uint32_t SwapPixels(const std::vector<uint8_t>& from, const std::vector<uint8_t>& to) {
    const uint8_t* a = from.data() + kHeader;
    const uint8_t* b = to.data() + kHeader;
    const uint32_t n = kSide * kSide, end = mem::kContiguousBase + mem::kPhysicalSize;
    // a part of the pixels with letters in it, compared first
    uint32_t probe = 0;
    for (uint32_t k = 0; k + 64 <= n; k += 64)
        if (std::count(a + k, a + k + 64, uint8_t(0)) < 32) {
            probe = k;
            break;
        }
    uint32_t swapped = 0;
    for (uint32_t va = mem::kContiguousBase; va < end; va += 0x1000) {
        if (!mem::IsCommitted(va)) continue;
        for (uint32_t at = va; at < va + 0x1000; at += 128) {
            if (uint64_t(at) + n > end || !mem::IsCommitted(at + probe + 63) ||
                std::memcmp(kernel::HostPtr(at + probe), a + probe, 64))
                continue;
            if (!mem::IsCommitted(at + n - 1) || std::memcmp(kernel::HostPtr(at), a, n)) continue;
            std::memcpy(kernel::HostPtr(at), b, n);
            swapped++;
        }
    }
    return swapped;
}

// the font chosen put in place, its texture too once the game has read it
void Apply(uint32_t mode) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_glyphs0.empty()) return;
    // TimeSplitters 2's waits for the font's pak, or is not to be had
    if (mode == 1 && (g_pak.empty() || !Ts2Ready())) mode = g_pak.empty() ? g_mode : 0;
    if (mode == g_mode) return;
    SetTables(mode);
    if (!g_font.empty() && (mode == 1) != (g_mode == 1)) {
        uint32_t n = mode == 1 ? SwapPixels(g_impact, g_font) : SwapPixels(g_font, g_impact);
        LOGR("font: the texture changed in %u places", n);
    }
    g_mode = mode;
    LOGR("font: %s", mode == 1 ? "TimeSplitters 2's" : mode == 2 ? "Arial Bold" : "the game's");
}

}  // namespace

namespace game {

// game_info.cpp Loaded: the game's tables kept, the font chosen put in place as far as it can be before the game runs
void InitFont() {
    auto* glyphs = static_cast<uint8_t*>(kernel::HostPtr(kGlyphs));
    auto* styles = static_cast<uint8_t*>(kernel::HostPtr(kStyles));
    g_glyphs0.assign(glyphs, glyphs + kGlyphCount * kGlyphSize);
    g_styles0.assign(styles, styles + kStyleCount * kStyleSize);
    g_spacing0 = kernel::R32(kImpact + kSpacingTable);
    g_kerning0 = kernel::R32(kImpact + kKerning);
    Apply(Opt().font);
    // the font's pak read: TimeSplitters 2's font goes over Impact's file as it comes in
    kernel::g_read_patch = [](const std::string& file, uint64_t pos, uint8_t* bytes, uint32_t n) {
        static const std::string kPakEnd = "arcade\\l_103.pak";
        if (file.size() < kPakEnd.size() || _stricmp(file.c_str() + file.size() - kPakEnd.size(), kPakEnd.c_str()))
            return;
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_pak.empty()) {
            g_pak = file;
            Apply(Opt().font);
        }
        if (g_mode != 1 || g_font.empty()) return;
        uint64_t from = std::max(pos, g_font_at), to = std::min(pos + n, g_font_at + g_font.size());
        if (from < to) std::memcpy(bytes + (from - pos), &g_font[from - g_font_at], size_t(to - from));
    };
}

// the settings panel: the font changed while the game runs
void ApplyFont() { Apply(Opt().font); }

// where TimeSplitters 2's files are read: the folder chosen, else the ts2 folder beside the game (tools/ts2_pack.py:
// only the HUD's textures, the font and its glyphs), else none
fs::path Ts2Root() {
    const std::string& folder = Opt().ts2_folder;
    if (!folder.empty()) return fs::u8path(folder);
    std::error_code ec;
    fs::path packed = app::ExeDir() / "ts2";
    return fs::is_directory(packed / "data", ec) ? packed : fs::path();
}

// TimeSplitters 2's files of these names (as its paks name them); none without a folder
std::map<std::string, std::vector<uint8_t>> Ts2Files(const std::set<std::string>& names) {
    fs::path root = Ts2Root();
    if (root.empty()) return {};
    return Ts2PakFiles(root / "data", names);
}

}  // namespace game
