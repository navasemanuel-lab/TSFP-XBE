// The early Future Perfect's skyline around Breaking and Entering's rooftop (level 6): the yellow towers at sunset
// with the NEW-GEN, SIRIUS, PLAZA and SUN-STAR signs, which the final game rebuilt. They are four props
// (nearfuture1/building{north,south,east,west}) in the player's own copy of the November 2004 preview for PS2, read
// from its disc image (or a folder with its files, or the preview folder a release has beside the game) and converted
// to this game's Xbox files when first asked for. When chosen, the game is given the beta's names for the final ones
// in the path it builds with "assets/ts/models/%s/xbox/output/%s.xbr" (game/beta_cortez.cpp's sprintf hook), and
// those names are served where the paks lack them, as the beta Cortez's are; so a level started after the choice has it.
//
// A preview model (.raw, little-endian): {texture table, model header, table of 0x30 byte entries} as on Xbox, with
// nodes of 0xB0 bytes (Xbox 0xC8: its fields from +0x3C are the preview's 0xC further on). A node's geometry is in
// chunks, VIF packets for the PS2's vector unit: {group, vertices, kind, index, first vertex} and pointers to the
// UNPACK data of the GIF tag, positions (V4-16 s16 /1024 plus the header's offset, or V4-32 floats; w carries the
// strip flags: 0x8000 a vertex not drawn, as strips start, and bit 0 the strip's winding reversed), UVs (V2-16 /1024),
// normals (V4-8) and colours (V4-8, as Xbox has them). Xbox has the node's own arrays of these and u16 index lists of
// strips, one for each run of chunks of a group, on its last chunk. Checked on the props unchanged since the preview:
// buildext comes out byte for byte the shipped one and 360 more draw the same triangles (and their UV animations
// are the shipped ones too).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kPaks[] = {"PAK/STORY/L_6_ST.PAK", "PAK/STORY/L_6_STM.PAK"};
constexpr const char* kBuildings[] = {"buildingnorth", "buildingsouth", "buildingeast", "buildingwest"};
constexpr char kFolder[] = "nearfuture1/", kSuffix[] = "beta";
constexpr uint32_t kPs2Node = 0xB0, kXbNode = 0xC8;
constexpr uint32_t kCollision[] = {12, 12, 1, 2};  // the element size of a node's four collision arrays

uint32_t Crc32(const std::string& s) {
    uint32_t c = ~0u;
    for (char ch : s) {
        c ^= uint8_t(ch);
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// little-endian bytes; out of range read as 0
struct Bytes {
    const std::vector<uint8_t>& d;
    bool In(size_t at, size_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint32_t U32(size_t at) const {
        uint32_t v = 0;
        if (In(at, 4)) std::memcpy(&v, &d[at], 4);
        return v;
    }
    uint16_t U16(size_t at) const { return In(at, 2) ? uint16_t(d[at] | d[at + 1] << 8) : 0; }
    int16_t S16(size_t at) const { return int16_t(U16(at)); }
    float F32(size_t at) const { return std::bit_cast<float>(U32(at)); }
    uint8_t U8(size_t at) const { return In(at, 1) ? d[at] : 0; }
};

struct Out {
    std::vector<uint8_t> b;
    void Align(size_t a) {
        while (b.size() % a) b.push_back(0);
    }
    uint32_t Put(const void* p, size_t n) {
        uint32_t at = uint32_t(b.size());
        b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return at;
    }
    template <class T>
    uint32_t Put(const std::vector<T>& v) {
        return Put(v.data(), v.size() * sizeof(T));
    }
    void Set32(uint32_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }
};

// the preview's disc: its image (ISO 9660) or a folder of its files
class Disc {
public:
    explicit Disc(const std::string& path) : path_(fs::u8path(path)) {
        std::error_code ec;
        iso_ = fs::is_regular_file(path_, ec);
    }
    // the bytes [offset, offset + size) of a file by its path on the disc ("PAK/STORY/L_6_ST.PAK")
    bool Read(const std::string& name, uint64_t offset, uint32_t size, std::vector<uint8_t>& out) {
        uint64_t at = 0, length = 0;
        fs::path file = path_;
        if (iso_) {
            if (!Find(name, at, length)) return false;
        } else {
            file /= fs::u8path(name);
            std::error_code ec;
            length = fs::file_size(file, ec);
            if (ec) return false;
        }
        if (offset > length || size > length - offset) return false;
        std::ifstream f(file, std::ios::binary);
        out.resize(size);
        return f && f.seekg(std::streamoff(at + offset)) && f.read(reinterpret_cast<char*>(out.data()), size);
    }

private:
    bool Sectors(uint64_t lba, uint32_t bytes, std::vector<uint8_t>& out) {
        std::ifstream f(path_, std::ios::binary);
        out.resize(bytes);
        return f && f.seekg(std::streamoff(lba * 2048)) && f.read(reinterpret_cast<char*>(out.data()), bytes);
    }
    // a file's extent, walking the directories from the primary volume descriptor's root
    bool Find(const std::string& name, uint64_t& at, uint64_t& length) {
        std::vector<uint8_t> s;
        if (!Sectors(16, 2048, s) || s[0] != 1 || std::memcmp(&s[1], "CD001", 5)) return false;
        Bytes pvd{s};
        uint32_t lba = pvd.U32(156 + 2), size = pvd.U32(156 + 10);
        size_t from = 0;
        while (from <= name.size()) {
            size_t to = name.find('/', from);
            std::string part = name.substr(from, to == std::string::npos ? std::string::npos : to - from);
            std::vector<uint8_t> dir;
            if (size > (1u << 20) || !Sectors(lba, size, dir)) return false;
            Bytes d{dir};
            bool found = false;
            for (uint32_t p = 0; p < size;) {
                uint8_t len = d.U8(p);
                if (!len) {  // records do not cross sectors
                    p = (p / 2048 + 1) * 2048;
                    continue;
                }
                std::string n(reinterpret_cast<const char*>(&dir[std::min<size_t>(p + 33, dir.size())]),
                              std::min<size_t>(d.U8(p + 32), dir.size() - std::min<size_t>(p + 33, dir.size())));
                if (size_t semi = n.find(';'); semi != std::string::npos) n.resize(semi);
                if (n.size() == part.size() &&
                    std::equal(n.begin(), n.end(), part.begin(), [](char a, char b) { return std::toupper(a) == std::toupper(b); })) {
                    lba = d.U32(p + 2);
                    size = d.U32(p + 10);
                    found = true;
                    break;
                }
                p += len;
            }
            if (!found) return false;
            if (to == std::string::npos) break;
            from = to + 1;
        }
        at = uint64_t(lba) * 2048;
        length = size;
        return true;
    }

    fs::path path_;
    bool iso_ = false;
};

// the preview's paks (P5CK: directory {crc32(path), offset, size, 0} at the header's +4, its size at +8)
class Paks {
public:
    explicit Paks(Disc& disc) : disc_(disc) {
        for (const char* pak : kPaks) {
            std::vector<uint8_t> head, dir;
            if (!disc_.Read(pak, 0, 12, head) || std::memcmp(head.data(), "P5CK", 4)) continue;
            Bytes h{head};
            if (h.U32(8) > (1u << 24) || !disc_.Read(pak, h.U32(4), h.U32(8), dir)) continue;
            Bytes d{dir};
            for (uint32_t e = 0; e + 16 <= dir.size(); e += 16) files_.emplace(d.U32(e), Entry{pak, d.U32(e + 4), d.U32(e + 8)});
        }
    }
    bool Read(const std::string& path, std::vector<uint8_t>& out) {
        auto it = files_.find(Crc32(path));
        return it != files_.end() && disc_.Read(it->second.pak, it->second.offset, it->second.size, out);
    }
    bool Any() const { return !files_.empty(); }

private:
    struct Entry {
        const char* pak;
        uint32_t offset, size;
    };
    Disc& disc_;
    std::map<uint32_t, Entry> files_;
};

// a chunk of a node: what its run table says and its vertices
struct Chunk {
    uint16_t grp = 0;
    uint8_t n = 0, kind = 0;
    uint16_t idx = 0;
    std::array<uint32_t, 4> gif{};
    std::vector<std::array<float, 3>> pos;
    std::vector<uint32_t> flags;  // the positions' w
    uint32_t uv = 0, nrm = 0, col = 0;
};

bool ReadChunks(const Bytes& b, uint32_t p, float scale_off[3], std::vector<Chunk>& out) {
    for (; p; p += 0x1C) {
        if (!b.In(p, 0x1C) || out.size() > 1024) return false;
        Chunk c{b.U16(p), b.U8(p + 2), b.U8(p + 3), b.U16(p + 4)};
        if (c.kind == 0xFF) return true;
        uint32_t gif = b.U32(p + 8), pos = b.U32(p + 12);
        c.uv = b.U32(p + 16), c.nrm = b.U32(p + 20), c.col = b.U32(p + 24);
        if (!gif || !pos || !c.uv || !b.In(gif, 16)) return false;
        for (int k = 0; k < 4; k++) c.gif[k] = b.U32(gif + 4 * k);
        // each array's VIF UNPACK code just before it: V4-32 0x6C, V4-16 0x6D, V4-8 0x6E, V2-16 0x75
        uint8_t fmt = b.U8(pos - 1);
        if (b.U8(c.uv - 1) != 0x75 || (c.nrm && b.U8(c.nrm - 1) != 0x6E) || (c.col && b.U8(c.col - 1) != 0x6E)) return false;
        if (!b.In(c.uv, c.n * 4u) || (c.nrm && !b.In(c.nrm, c.n * 4u)) || (c.col && !b.In(c.col, c.n * 4u))) return false;
        for (uint32_t k = 0; k < c.n; k++) {
            if (fmt == 0x6D && b.In(pos + 8 * k, 8)) {
                uint32_t q = pos + 8 * k;
                c.pos.push_back({b.S16(q) / 1024.0f + scale_off[0], b.S16(q + 2) / 1024.0f + scale_off[1],
                                 b.S16(q + 4) / 1024.0f + scale_off[2]});
                c.flags.push_back(b.U16(q + 6));
            } else if (fmt == 0x6C && b.In(pos + 16 * k, 16)) {
                uint32_t q = pos + 16 * k;
                c.pos.push_back({b.F32(q), b.F32(q + 4), b.F32(q + 8)});
                c.flags.push_back(b.U32(q + 12));
            } else {
                return false;
            }
        }
        out.push_back(std::move(c));
    }
    return true;
}

// a chunk's strips out of its vertices' flags (0x8000: not drawn), as local indices
std::vector<std::vector<uint32_t>> Strips(const Chunk& c) {
    std::vector<std::vector<uint32_t>> out;
    std::vector<uint32_t> cur;
    auto end = [&] {
        if (cur.size() >= 3) out.push_back(cur);
    };
    for (uint32_t i = 0; i < c.n; i++) {
        bool skip = c.flags[i] & 0x8000, next = i + 1 < c.n && (c.flags[i + 1] & 0x8000);
        if (skip && (next || cur.empty() || i + 1 == c.n)) {
            end();
            cur = {i};
        } else if (skip) {  // one vertex not drawn inside a strip: a new one from the vertex before
            end();
            cur = {i - 1, i};
        } else {
            cur.push_back(i);
        }
    }
    end();
    return out;
}

uint32_t PackNormal(const float n[3]) {
    auto q = [](float v, int bits) {
        int m = (1 << (bits - 1)) - 1;
        int i = int(std::lround(std::clamp(v, -1.0f, 1.0f) * m));
        return uint32_t(i) & ((1u << bits) - 1);
    };
    return q(n[0], 11) | q(n[1], 11) << 11 | q(n[2], 10) << 22;
}

// preview .raw -> Xbox .xbr
bool Convert(const std::vector<uint8_t>& raw, std::vector<uint8_t>& xbr) {
    Bytes b{raw};
    uint32_t tex = b.U32(0), hdr = b.U32(4), tab30 = b.U32(8);
    uint32_t nodes = b.U32(hdr);
    if (!b.In(hdr, 17 * 4) || !nodes || nodes > 256 || hdr < nodes * kPs2Node) return false;
    uint32_t nodebase = hdr - nodes * kPs2Node;
    float off[3] = {b.F32(hdr + 44), b.F32(hdr + 48), b.F32(hdr + 52)};
    Out o;
    for (uint32_t v : {0xCu, 0u, 0u}) o.Put(&v, 4);
    for (uint32_t p = tex;; p += 16) {
        if (!b.In(p, 16)) return false;
        o.Put(&raw[p], 16);
        if (b.U32(p) == 0xFFFFFFFF) break;
    }
    std::vector<std::map<uint32_t, uint32_t>> ptrs(nodes);  // a node's Xbox pointer fields
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t node = nodebase + i * kPs2Node;
        auto w = [&](uint32_t f) { return b.U32(node + f); };
        // pointers whose Xbox layout this does not know: a third chunk set, boxes and others
        for (uint32_t f : {0x5Cu, 0x60u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu, 0x80u})
            if (w(f)) return false;
        auto& ptr = ptrs[i];
        // the chunk sets: {preview field, Xbox render state, positions, UVs, normals, colours, run table, strips}
        struct Set {
            uint32_t from, rs, pos, uv, nrm, col, run, tab;
        };
        for (const Set& s : {Set{0x54, 0x00, 0x04, 0x08, 0x0C, 0x10, 0x60, 0xBC}, Set{0x58, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x64, 0xC0}}) {
            std::vector<Chunk> ch;
            if (!ReadChunks(b, w(s.from), off, ch)) return false;
            if (ch.empty()) continue;
            bool col = ch[0].col, nrm = ch[0].nrm;
            for (const Chunk& c : ch)
                if (bool(c.col) != col || bool(c.nrm) != nrm) return false;
            // render state: each chunk's GIF tag, every word four times
            std::vector<uint32_t> rs;
            for (const Chunk& c : ch)
                for (uint32_t word : c.gif) rs.insert(rs.end(), 4, word);
            ptr[s.rs] = o.Put(rs);
            if (col) {
                ptr[s.col] = uint32_t(o.b.size());
                for (const Chunk& c : ch) o.Put(&raw[c.col], c.n * 4u);
            }
            if (nrm) {
                std::vector<uint32_t> nr;
                for (const Chunk& c : ch)
                    for (uint32_t k = 0; k < c.n; k++) {
                        float n[3] = {int8_t(raw[c.nrm + 4 * k]) / 127.0f, int8_t(raw[c.nrm + 4 * k + 1]) / 127.0f,
                                      int8_t(raw[c.nrm + 4 * k + 2]) / 127.0f};
                        nr.insert(nr.end(), {PackNormal(n), 0u});
                    }
                ptr[s.nrm] = o.Put(nr);
            }
            o.Align(16);
            std::vector<float> pos, uv;
            for (const Chunk& c : ch)
                for (uint32_t k = 0; k < c.n; k++) {
                    pos.insert(pos.end(), {c.pos[k][0], c.pos[k][1], c.pos[k][2], std::bit_cast<float>(0xFFFFFFE0u)});
                    uv.insert(uv.end(), {b.S16(c.uv + 4 * k) / 1024.0f, b.S16(c.uv + 4 * k + 2) / 1024.0f, 1.0f});
                }
            ptr[s.pos] = o.Put(pos);
            ptr[s.uv] = o.Put(uv);
            // index lists: a run of chunks of one group drawn as one list, on its last chunk; strips joined by
            // repeated vertices, each starting on the triangle parity of its winding
            std::vector<uint32_t> starts;
            uint32_t total = 0;
            for (const Chunk& c : ch) starts.push_back(total), total += c.n;
            if (total > 0xFFFF) return false;
            uint32_t tab = o.Put(std::vector<uint32_t>(2 * ch.size()));
            ptr[s.tab] = tab;
            o.Align(16);
            for (size_t k = 0; k < ch.size();) {
                size_t e = k;
                while (e + 1 < ch.size() && ch[e + 1].grp == ch[k].grp) e++;
                std::vector<uint16_t> idx;
                for (size_t j = k; j <= e; j++)
                    for (auto& st : Strips(ch[j])) {
                        size_t flip = ch[j].flags[st[2]] & 1;
                        if (!idx.empty()) idx.insert(idx.end(), {idx.back(), uint16_t(starts[j] + st[0])});
                        if (idx.size() % 2 != flip) idx.push_back(uint16_t(starts[j] + st[0]));
                        for (uint32_t v : st) idx.push_back(uint16_t(starts[j] + v));
                    }
                if (!idx.empty()) {
                    o.Set32(tab + 8 * uint32_t(e), o.Put(idx));
                    o.Set32(tab + 8 * uint32_t(e) + 4, uint32_t(idx.size()));
                }
                k = e + 1;
            }
            o.Align(4);
            ptr[s.run] = uint32_t(o.b.size());
            for (size_t j = 0; j < ch.size(); j++) {
                uint16_t rec[5] = {ch[j].grp, uint16_t(ch[j].n | ch[j].kind << 8), ch[j].idx, uint16_t(starts[j]), 0xFFFF};
                o.Put(rec, sizeof(rec));
            }
            uint16_t last[5] = {0, 0xFF00, 0, 0, 0xFFFF};
            o.Put(last, sizeof(last));
        }
        // collision: a header of bounds and four (count, pointer) arrays
        if (uint32_t c = w(0x9C)) {
            if (!b.In(c, 0x38)) return false;
            std::vector<uint8_t> head(raw.begin() + c, raw.begin() + c + 0x38);
            Bytes h{head};
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t n = h.U32(0x18 + 8 * k), p = h.U32(0x1C + 8 * k);
                if (!n || !p) continue;
                if (!b.In(p, size_t(n) * kCollision[k])) return false;
                o.Align(4);
                uint32_t at = o.Put(&raw[p], size_t(n) * kCollision[k]);
                std::memcpy(&head[0x1C + 8 * k], &at, 4);
            }
            o.Align(4);
            ptr[0xA8] = o.Put(head);
        }
    }
    o.Align(4);
    for (uint32_t i = 0; i < nodes; i++) {
        uint32_t node = nodebase + i * kPs2Node;
        std::vector<uint8_t> rec(kXbNode);
        for (uint32_t f = 0x3C; f < kPs2Node; f += 4) {
            if (f == 0x54 || f == 0x58 || f == 0x9C) continue;
            uint32_t v = b.U32(node + f);
            std::memcpy(&rec[f + 0xC], &v, 4);
        }
        for (auto [f, v] : ptrs[i]) std::memcpy(&rec[f], &v, 4);
        o.Put(rec);
    }
    auto hw = [&](uint32_t k) { return b.U32(hdr + 4 * k); };
    uint32_t head[19] = {hw(0), hw(1), hw(2), hw(3) & 0xCF, 0, 0, 0, 0, 0, 0, 0, 0, hw(9), hw(10), hw(11), hw(12), hw(13), hw(14), 0x10};
    uint32_t at_hdr = o.Put(head, sizeof(head));
    // materials: {4 texture indices, ..., flags +0x1C, UV animations +0x20..+0x2C}; an animation is 5 pointers to
    // curves {count, count words} (the scrolling signs), which move with the rest
    uint32_t end = tab30;
    while (b.In(end, 0x30) && b.U32(end) != 0xFFFFFFFF) end += 0x30;
    if (!b.In(end, 0x30)) return false;
    std::vector<uint8_t> mats(raw.begin() + tab30, raw.begin() + end + 0x30);
    for (uint32_t e = 0; e + 0x30 < mats.size(); e += 0x30)
        for (uint32_t k = 0x20; k < 0x30; k += 4) {
            uint32_t p = b.U32(tab30 + e + k);
            if (!p) continue;
            if (!b.In(p, 20)) return false;
            uint32_t anim[5];
            for (uint32_t j = 0; j < 5; j++) {
                uint32_t q = b.U32(p + 4 * j), n = b.U32(q);
                anim[j] = 0;
                if (!q) continue;
                if (n > 4096 || !b.In(q, 4 + 4 * size_t(n))) return false;
                anim[j] = o.Put(&raw[q], 4 + 4 * size_t(n));
            }
            uint32_t at = o.Put(anim, sizeof(anim));
            std::memcpy(&mats[e + k], &at, 4);
        }
    o.Align(4);
    uint32_t at_tab30 = o.Put(mats);
    o.Set32(4, at_hdr);
    o.Set32(8, at_tab30);
    xbr = std::move(o.b);
    return true;
}

// preview texture (5, 1, w, h; 256 colours RGBA in the PS2's CLUT order, alpha 0x80 opaque; w*h indices stored as
// PSMT8 in PSMCT32 blocks) -> Xbox .xbt A8R8G8B8 (format 2), swizzled
bool ConvertTexture(const std::vector<uint8_t>& raw, std::vector<uint8_t>& xbt) {
    Bytes b{raw};
    uint32_t w = b.U32(8), h = b.U32(12);
    if (b.U32(0) != 5 || w < 16 || h < 16 || w > 2048 || h > 2048 || (w & (w - 1)) || (h & (h - 1)) ||
        !b.In(16 + 1024, size_t(w) * h))
        return false;
    const uint8_t* pal = &raw[16];
    const uint8_t* pix = &raw[16 + 1024];
    xbt.assign(0x80 + size_t(w) * h * 4, 0);
    uint32_t head[7] = {w, h, w, h, 0, 2, 0xFFFFFFFF};
    std::memcpy(xbt.data(), head, sizeof(head));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t block = (y & ~0xFu) * w + (x & ~0xFu) * 2, swap = (((y + 2) >> 2) & 1) * 4;
            uint32_t py = (((y & ~3u) >> 1) + (y & 1)) & 7;
            uint32_t at = block + py * w * 2 + ((x + swap) & 7) * 4 + ((y >> 1) & 1) + ((x >> 2) & 2);
            uint32_t i = pix[at], j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);
            const uint8_t* c = &pal[j * 4];
            uint32_t s = 0, bit = 0;  // Xbox swizzle: x and y bits interleaved from the lowest, x first
            for (uint32_t k = 0; (1u << k) < w || (1u << k) < h; k++) {
                if ((1u << k) < w) s |= ((x >> k) & 1) << bit++;
                if ((1u << k) < h) s |= ((y >> k) & 1) << bit++;
            }
            uint8_t* d = &xbt[0x80 + size_t(s) * 4];
            d[0] = c[2], d[1] = c[1], d[2] = c[0], d[3] = uint8_t(std::min(255, c[3] * 2));
        }
    return true;
}

// the converted files by path, built once
std::map<std::string, std::vector<uint8_t>> g_files;
std::map<std::string, bool> g_ready;  // a building's beta name: built
std::once_flag g_built;
std::atomic<bool> g_done = false;  // the two above complete, never changed again

std::string ModelPath(const std::string& name, const std::string& file, const char* platform, const char* ext) {
    return "assets/ts/models/" + name + "/" + platform + "/output/" + file + "." + ext;
}

void Build() {
    // the disc image or folder chosen, else the preview folder beside the game (tools/preview_pack.py: only the
    // buildings and their textures)
    std::string path = game::Opt().preview_path;
    std::error_code ec;
    if (path.empty() && fs::is_directory(app::ExeDir() / "preview", ec)) path = (app::ExeDir() / "preview").string();
    if (path.empty()) return;
    Disc disc(path);
    Paks paks(disc);
    if (!paks.Any()) {
        LOGR("beta skyline: no PAK/STORY/L_6_ST.PAK in %s", path.c_str());
        return;
    }
    for (const char* building : kBuildings) {
        std::string name = std::string(kFolder) + building;
        std::vector<uint8_t> raw, xbr;
        if (!paks.Read(ModelPath(name, building, "ps2", "raw"), raw) || !Convert(raw, xbr)) {
            LOGR("beta skyline: %s not read or not converted", building);
            continue;
        }
        std::map<std::string, std::vector<uint8_t>> textures;
        bool ok = true;
        Bytes b{raw};
        for (uint32_t p = b.U32(0); ok && b.In(p, 16) && b.U32(p) != 0xFFFFFFFF; p += 16) {
            char id[16];
            std::snprintf(id, sizeof(id), "%06x", b.U32(p));
            std::vector<uint8_t> tex, xbt;
            if (!paks.Read(std::string("textures/") + id + ".raw", tex) || !ConvertTexture(tex, xbt)) {
                LOGR("beta skyline: %s texture %s not read or not converted", building, id);
                ok = false;
                break;
            }
            textures[std::string("textures/") + id + ".xbt"] = std::move(xbt);
        }
        if (!ok) continue;
        LOGR("beta skyline: %s built, %zu bytes, %zu textures", building, xbr.size(), textures.size());
        std::string beta = std::string(building) + kSuffix;
        g_files[ModelPath(std::string(kFolder) + beta, beta, "xbox", "xbr")] = std::move(xbr);
        g_files.merge(textures);
        g_ready[std::string(kFolder) + beta] = true;
    }
    g_done = true;
}

}  // namespace

namespace game {

// a file of the beta skyline the paks lack (path as the beta Cortez's are asked for), once built
const std::vector<uint8_t>* BetaSkylineFile(const std::string& path) {
    if (!g_done) return nullptr;
    auto it = g_files.find(path);
    return it == g_files.end() ? nullptr : &it->second;
}

// a model's names for the path the game builds (name "nearfuture1/buildingeast", file "buildingeast"): the beta's
// in their place when it is chosen and built, as guest strings; false to keep the game's
bool BetaSkylineNames(const std::string& name, uint32_t& name_at, uint32_t& file_at) {
    if (Opt().skyline_model != 1 || name.compare(0, sizeof(kFolder) - 1, kFolder)) return false;
    std::string file = name.substr(sizeof(kFolder) - 1);
    if (std::find(std::begin(kBuildings), std::end(kBuildings), file) == std::end(kBuildings)) return false;
    std::call_once(g_built, Build);
    std::string beta = name + kSuffix;
    if (!g_ready.count(beta)) return false;
    static std::mutex lock;
    static std::map<std::string, uint32_t> strings;  // each beta's two names in guest memory, made once
    std::lock_guard guard(lock);
    uint32_t& at = strings[beta];
    if (!at) {
        if (!(at = mem::AllocSystem(128))) return false;
        std::string short_name = file + kSuffix;
        std::memcpy(kernel::HostPtr(at), beta.c_str(), beta.size() + 1);
        std::memcpy(kernel::HostPtr(at + 64), short_name.c_str(), short_name.size() + 1);
        LOGR("beta skyline: %s loaded as %s", name.c_str(), beta.c_str());
    }
    name_at = at;
    file_at = at + 64;
    return true;
}

}  // namespace game
