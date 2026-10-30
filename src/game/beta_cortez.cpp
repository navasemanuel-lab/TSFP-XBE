// The early Future Perfect's Cortez (teal armour, the 2004 look), from the player's own copy of the leaked
// TimeSplitters 4 demo for GameCube: its cortezingame.gcr and textures, converted to this game's Xbox files when first
// asked for (never shipped with this port). The engine still names the model (CHR_CASTLE_CORTEZ, chrs/cortezingame)
// though its file is in no pak; it is served where the paks lack it (game/briefing.cpp's file hooks), and in Story it
// can take the place of the final Cortez (chrs/cortezarmouredingame and its LOD), the path the game builds for those
// with "assets/ts/models/%s/xbox/output/%s.xbr" given the beta's names instead.
//
// A model (.gcr GameCube, big-endian, and .xbr Xbox, as 0x8DF60 fixes it up): {texture table at 0xC, model header,
// table of 0x30 byte entries}; the header {nodes, matrices, 1, flags, ...} follows the nodes (Xbox 0xC8 bytes each,
// GameCube 0xC0). A mesh node's vertices are on GameCube global arrays of s16 positions (/1024, plus the header's
// offset), u16 UVs (/1024) and indices into the engine's own normal table, drawn as GX display lists of tristrips
// with a matrix slot each vertex; on Xbox they are the node's own arrays (positions with the matrix's palette entry as
// a tag, UVs, packed normals) and u16 index lists. The normal table is in no file: it is rebuilt from the TimeSplitters
// 4 models this game also has (chr.pak), vertex by vertex, and from the faces where none tells.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/mods.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kModelPath = 0x478354;    // "assets/ts/models/%s/xbox/output/%s.xbr"
constexpr char kChrPak[] = "D:\\pak\\chr.pak";
constexpr char kBetaName[] = "chrs/cortezingame", kBetaFile[] = "cortezingame";
constexpr char kBetaPath[] = "assets/ts/models/chrs/cortezingame/xbox/output/cortezingame.xbr";
// the beta's cutscene model (40 matrices, its textures in the file), which this game ships as Xbox in one level's pak
constexpr char kCsName[] = "chrs/cortezingamecs", kCsFile[] = "cortezingamecs";
constexpr char kCsPath[] = "assets/ts/models/chrs/cortezingamecs/xbox/output/cortezingamecs.xbr";
constexpr char kCsPak[] = "D:\\pak\\stream\\lv4\\propcs.pak";
constexpr uint32_t kMap = 0x79094C;
// the cutscene maps, which 0x1CEB00 tells (and whose paks are pak/cutscene/l_<map>_cs.pak)
constexpr int32_t kCutsceneMaps[] = {8, 16, 19, 23, 24, 30, 32, 33, 34, 35, 36, 37, 40, 41, 43, 44, 45, 47, 57, 82};
// the final Cortez's model and LOD, which the beta replaces in Story
constexpr const char* kFinal[] = {"chrs/cortezarmouredingame", "chrs/cortezarmouredingamelod"};
constexpr uint32_t kXbNode = 0xC8, kGcNode = 0xC0;
constexpr uint32_t kSlots = 10;  // GX position matrices

// files
bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

uint32_t Crc32(const char* s) {
    uint32_t c = ~0u;
    for (; *s; s++) {
        c ^= uint8_t(*s);
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// bytes, out of range read as 0
struct Bytes {
    const std::vector<uint8_t>& d;
    bool big;
    bool In(size_t at, size_t n) const { return at + n <= d.size(); }
    uint32_t U32(size_t at) const {
        if (!In(at, 4)) return 0;
        const uint8_t* p = &d[at];
        return big ? uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3] : uint32_t(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0];
    }
    uint16_t U16(size_t at) const {
        if (!In(at, 2)) return 0;
        return big ? uint16_t(d[at] << 8 | d[at + 1]) : uint16_t(d[at + 1] << 8 | d[at]);
    }
    float F32(size_t at) const { return std::bit_cast<float>(U32(at)); }
};

// a strip's vertex on GameCube: {matrix (PNMTX, 3 a slot), position, normal, UV}
struct GcVertex {
    uint8_t mtx;
    uint16_t pos, nrm, uv;
    auto operator<=>(const GcVertex&) const = default;
};
using Strip = std::vector<GcVertex>;

// a chunk: {group, vertices, kind, index, first vertex}
struct Chunk {
    uint16_t grp;
    uint8_t n, kind;
    uint16_t idx, start;
};

struct Mesh {
    uint32_t pos = 0, uv = 0, nrm = 0;  // GameCube: the global arrays; Xbox: the node's own
    std::vector<Chunk> chunks;
    std::vector<std::vector<Strip>> strips;  // GameCube: a chunk's display list
    uint32_t vertices = 0;                   // Xbox
};

struct Model {
    uint32_t hdr = 0, tab30 = 0, nodes = 0, nodebase = 0;
    std::array<float, 3> off{};
    std::map<uint32_t, Mesh> meshes;
};

std::vector<Chunk> ReadChunks(const Bytes& b, uint32_t at, uint32_t size) {
    std::vector<Chunk> out;
    for (; b.In(at, size) && out.size() < 4096; at += size) {
        Chunk c{b.U16(at), b.d[at + 2], b.d[at + 3], b.U16(at + 4), b.U16(at + 6)};
        if (c.n == 0 && c.kind == 0xFF) break;
        out.push_back(c);
    }
    return out;
}

// a GX display list of tristrips (0x9D), padded with 0; false for anything else
bool GxStrips(const Bytes& b, uint32_t at, uint32_t size, std::vector<Strip>& out) {
    if (!b.In(at, size)) return false;
    for (uint32_t p = at, end = at + size; p < end;) {
        uint8_t op = b.d[p];
        if (op == 0) {
            p++;
            continue;
        }
        if (op != 0x9D || p + 3 > end) return false;
        uint32_t count = b.U16(p + 1);
        p += 3;
        if (p + count * 7 > end) return false;
        Strip s;
        for (uint32_t k = 0; k < count; k++, p += 7) s.push_back({b.d[p], b.U16(p + 1), b.U16(p + 3), b.U16(p + 5)});
        out.push_back(std::move(s));
    }
    return true;
}

bool ReadHeader(const Bytes& b, uint32_t node_size, Model& m) {
    m.hdr = b.U32(4);
    m.tab30 = b.U32(8);
    m.nodes = b.U32(m.hdr);
    if (!b.In(m.hdr, 0x40) || m.nodes == 0 || m.nodes > 512 || m.nodes * node_size > m.hdr) return false;
    m.nodebase = m.hdr - m.nodes * node_size;
    return true;
}

uint32_t Word(const Bytes& b, const Model& m, uint32_t node, uint32_t field, uint32_t node_size) {
    return b.U32(m.nodebase + node * node_size + field);
}

bool ReadGc(const Bytes& b, Model& m) {
    if (!ReadHeader(b, kGcNode, m)) return false;
    for (int k = 0; k < 3; k++) m.off[k] = b.F32(m.hdr + 0x2C + 4 * k);
    for (uint32_t i = 0; i < m.nodes; i++) {
        uint32_t st = Word(b, m, i, 0xB0, kGcNode);
        if (!st) continue;
        Mesh me;
        me.pos = Word(b, m, i, 4, kGcNode);
        me.uv = Word(b, m, i, 8, kGcNode);
        me.nrm = Word(b, m, i, 0xC, kGcNode);
        if (uint32_t ch = Word(b, m, i, 0x54, kGcNode)) me.chunks = ReadChunks(b, ch, 8);
        for (size_t s = 0; s < me.chunks.size(); s++) {
            uint32_t p = b.U32(st + s * 8), size = b.U32(st + s * 8 + 4);
            me.strips.emplace_back();
            if (size && !GxStrips(b, p, size, me.strips.back())) return false;
        }
        m.meshes[i] = std::move(me);
    }
    return true;
}

bool ReadXb(const Bytes& b, Model& m) {
    if (!ReadHeader(b, kXbNode, m)) return false;
    for (uint32_t i = 0; i < m.nodes; i++) {
        if (!Word(b, m, i, 0xBC, kXbNode)) continue;
        Mesh me;
        me.pos = Word(b, m, i, 4, kXbNode);
        me.uv = Word(b, m, i, 8, kXbNode);
        me.nrm = Word(b, m, i, 0xC, kXbNode);
        if (uint32_t ch = Word(b, m, i, 0x60, kXbNode)) me.chunks = ReadChunks(b, ch, 10);
        for (auto& c : me.chunks) me.vertices += c.n;
        if (!b.In(me.pos, me.vertices * 16) || !b.In(me.uv, me.vertices * 12) || !b.In(me.nrm, me.vertices * 8))
            return false;
        m.meshes[i] = std::move(me);
    }
    return true;
}

// GameCube matrix slots: which of a node's palette entries (the Xbox tag's index) each PNMTX slot holds. A node's
// palette is its bytes +0x3C..+0x44: {kind, own, a, b, ?, c}, of node ids (a node's id its own byte 1); the slots are
// a cache of 10 filled in node order, emptied when a node's new ids do not fit. Each node's palette (matrix ids) in
// palettes.
std::map<uint32_t, std::map<uint32_t, uint32_t>> MatrixSlots(const Bytes& b, const Model& m,
                                                             std::map<uint32_t, std::vector<int>>& palettes) {
    auto pal = [&](uint32_t node, int k) {
        uint32_t at = m.nodebase + node * kGcNode + 0x3C + uint32_t(k);
        return b.In(at, 1) ? b.d[at] : uint8_t(0);
    };
    auto nid = [&](uint32_t node) { return node < m.nodes ? pal(node, 1) : uint8_t(0); };
    std::map<uint32_t, std::map<uint32_t, uint32_t>> out;
    std::array<int, kSlots> cache;
    cache.fill(-1);
    uint32_t next = 0;
    for (auto& [i, me] : m.meshes) {
        std::vector<int> ids;
        if (pal(i, 0) == 0) {
            ids = {pal(i, 1)};
        } else {
            ids = {nid(pal(i, 2)), nid(pal(i, 3))};
            if (pal(i, 0) >= 2) ids.push_back(pal(i, 1));
            if (pal(i, 0) >= 3 && pal(i, 5) < m.nodes) ids.push_back(nid(pal(i, 5)));  // 0xFF: none
        }
        palettes[i] = ids;
        auto cached = [&](int x) { return std::find(cache.begin(), cache.end(), x); };
        size_t need = std::count_if(ids.begin(), ids.end(), [&](int x) { return cached(x) == cache.end(); });
        if (next + need > kSlots) {
            cache.fill(-1);
            next = 0;
        }
        for (uint32_t j = 0; j < ids.size(); j++) {
            auto it = cached(ids[j]);
            if (it != cache.end()) {
                out[i][j] = uint32_t(it - cache.begin());
            } else if (next < kSlots) {
                cache[next] = ids[j];
                out[i][j] = next++;
            }
        }
    }
    return out;
}

// Xbox normals: 11:11:10 signed
std::array<float, 3> UnpackNormal(uint32_t n) {
    auto s = [](uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); };
    return {s(n & 0x7FF, 11) / 1023.0f, s((n >> 11) & 0x7FF, 11) / 1023.0f, s(n >> 22, 10) / 511.0f};
}

uint32_t PackNormal(const std::array<float, 3>& n) {
    auto q = [](float v, int bits) {
        int32_t m = (1 << (bits - 1)) - 1;
        int32_t i = int32_t(std::lround(std::clamp(v, -1.0f, 1.0f) * float(m)));
        return uint32_t(i) & ((1u << bits) - 1);
    };
    return q(n[0], 11) | q(n[1], 11) << 11 | q(n[2], 10) << 22;
}

using Normals = std::map<uint32_t, std::array<float, 3>>;

std::set<uint32_t> NormalIndices(const Model& m) {
    std::set<uint32_t> out;
    for (auto& [i, me] : m.meshes)
        for (auto& list : me.strips)
            for (auto& s : list)
                for (auto& v : s) out.insert(v.nrm);
    return out;
}

// the engine's normal table where the beta uses it: TimeSplitters 4's models that this game also has, their
// vertices matched by position and UV, each GameCube normal index the average of its Xbox vertices' normals
Normals NormalTable(const fs::path& models, const std::set<uint32_t>& wanted) {
    std::map<uint32_t, std::array<double, 4>> sum;
    std::vector<uint8_t> pak;
    if (!kernel::ReadGameFile(kChrPak, pak) || pak.size() < 16 || std::memcmp(pak.data(), "P5CK", 4)) {
        LOGR("beta cortez: no %s", kChrPak);
        return {};
    }
    Bytes pb{pak, false};
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> dir;  // {hash, offset, size, 0}
    for (uint32_t e = pb.U32(4), end = e + pb.U32(8); e + 16 <= end && pb.In(e, 16); e += 16)
        dir[pb.U32(e)] = {pb.U32(e + 4), pb.U32(e + 8)};
    std::set<uint32_t> missing = wanted;
    uint32_t used = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(models / "chrs", ec); !ec && it != fs::directory_iterator() && !missing.empty();
         it.increment(ec)) {
        std::string name = it->path().filename().string();  // the game's names are ASCII
        std::string xb_path = "assets/ts/models/chrs/" + name + "/xbox/output/" + name + ".xbr";
        auto found = dir.find(Crc32(xb_path.c_str()));
        if (found == dir.end() || !pb.In(found->second.first, found->second.second)) continue;
        std::vector<uint8_t> gc_file;
        if (!ReadFile(it->path() / "gamecube" / "output" / (name + ".gcr"), gc_file)) continue;
        Bytes gb{gc_file, true};
        Model g;
        if (!ReadGc(gb, g)) continue;
        std::set<uint32_t> has = NormalIndices(g);
        if (std::none_of(has.begin(), has.end(), [&](uint32_t n) { return missing.count(n); })) continue;
        std::vector<uint8_t> xb_file(pak.begin() + found->second.first,
                                     pak.begin() + found->second.first + found->second.second);
        Bytes xb{xb_file, false};
        Model x;
        if (!ReadXb(xb, x) || x.nodes != g.nodes) continue;
        for (auto& [i, gm] : g.meshes) {
            auto xm = x.meshes.find(i);
            if (xm == x.meshes.end()) continue;
            // position and UV in 1/1024 -> the GameCube normal index, -1 when two tell apart
            std::map<std::array<int32_t, 5>, int32_t> key;
            for (auto& list : gm.strips)
                for (auto& s : list)
                    for (auto& v : s) {
                        std::array<int32_t, 5> k{int16_t(gb.U16(gm.pos + v.pos * 6u)),
                                                 int16_t(gb.U16(gm.pos + v.pos * 6u + 2)),
                                                 int16_t(gb.U16(gm.pos + v.pos * 6u + 4)), gb.U16(gm.uv + v.uv * 4u),
                                                 gb.U16(gm.uv + v.uv * 4u + 2)};
                        auto [at, fresh] = key.emplace(k, int32_t(v.nrm));
                        if (!fresh && at->second != int32_t(v.nrm)) at->second = -1;
                    }
            const Mesh& me = xm->second;
            for (uint32_t k = 0; k < me.vertices; k++) {
                std::array<int32_t, 5> q;
                for (int c = 0; c < 3; c++)
                    q[c] = int32_t(std::lround((xb.F32(me.pos + k * 16 + c * 4) - g.off[c]) * 1024.0f));
                for (int c = 0; c < 2; c++) q[3 + c] = int32_t(std::lround(xb.F32(me.uv + k * 12 + c * 4) * 1024.0f));
                auto at = key.find(q);
                if (at == key.end() || at->second < 0) continue;
                auto n = UnpackNormal(xb.U32(me.nrm + k * 8));
                auto& e = sum[uint32_t(at->second)];
                for (int c = 0; c < 3; c++) e[c] += n[c];
                e[3] += 1;
                missing.erase(uint32_t(at->second));
            }
        }
        used++;
    }
    Normals out;
    for (auto& [i, e] : sum) {
        double l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (l > 0) out[i] = {float(e[0] / l), float(e[1] / l), float(e[2] / l)};
    }
    size_t had = std::count_if(wanted.begin(), wanted.end(), [&](uint32_t n) { return out.count(n); });
    LOGR("beta cortez: %zu of %zu normals from %u models", had, wanted.size(), used);
    return out;
}

// normals the table lacks: the beta's own faces at the vertices that use them
void FaceNormals(const Bytes& b, const Model& m, const std::set<uint32_t>& wanted, Normals& normals) {
    std::map<uint32_t, std::array<double, 3>> sum;
    auto pos = [&](const Mesh& me, uint16_t p) {
        std::array<double, 3> v;
        for (int c = 0; c < 3; c++) v[c] = int16_t(b.U16(me.pos + p * 6u + c * 2u)) / 1024.0;
        return v;
    };
    for (auto& [i, me] : m.meshes)
        for (auto& list : me.strips)
            for (auto& s : list)
                for (size_t k = 2; k < s.size(); k++) {
                    auto a = pos(me, s[k - 2].pos), c = pos(me, s[k - 1].pos), d = pos(me, s[k].pos);
                    if (k % 2) std::swap(a, c);
                    std::array<double, 3> u{c[0] - a[0], c[1] - a[1], c[2] - a[2]}, w{d[0] - a[0], d[1] - a[1], d[2] - a[2]};
                    std::array<double, 3> n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
                    for (size_t t = k - 2; t <= k; t++)
                        if (!normals.count(s[t].nrm))
                            for (int q = 0; q < 3; q++) sum[s[t].nrm][q] += n[q];
                }
    uint32_t added = 0;
    for (uint32_t i : wanted) {
        if (normals.count(i)) continue;
        auto& e = sum[i];
        double l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        normals[i] = l > 0 ? std::array<float, 3>{float(e[0] / l), float(e[1] / l), float(e[2] / l)}
                           : std::array<float, 3>{0, 1, 0};
        added++;
    }
    if (added) LOGR("beta cortez: %u normals from the model's faces", added);
}

// the output file, little-endian
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
    uint32_t U32(uint32_t v) { return Put(&v, 4); }
    void Set32(uint32_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }
};

// big-endian words as little-endian ones
std::vector<uint8_t> Swap32(const std::vector<uint8_t>& d, uint32_t from, uint32_t to) {
    std::vector<uint8_t> out;
    for (uint32_t p = from; p + 4 <= to && p + 4 <= d.size(); p += 4) out.insert(out.end(), {d[p + 3], d[p + 2], d[p + 1], d[p]});
    return out;
}

// GameCube .gcr -> Xbox .xbr
bool Convert(const std::vector<uint8_t>& gc, const Normals& normals, std::vector<uint8_t>& out) {
    Bytes b{gc, true};
    Model g;
    if (!ReadGc(b, g)) return false;
    std::map<uint32_t, std::vector<int>> palettes;
    auto slots = MatrixSlots(b, g, palettes);
    auto word = [&](uint32_t node, uint32_t field) { return Word(b, g, node, field, kGcNode); };
    // every pointer, to size the opaque state blocks
    std::set<uint32_t> ptrs{g.hdr, g.tab30, uint32_t(gc.size())};
    for (uint32_t i = 0; i < g.nodes; i++) {
        for (uint32_t f : {0u, 0x54u, 0xA0u, 0xB0u, 4u, 8u}) ptrs.insert(word(i, f));
        if (auto me = g.meshes.find(i); me != g.meshes.end())
            for (size_t s = 0; s < me->second.chunks.size(); s++) ptrs.insert(b.U32(word(i, 0xB0) + s * 8));
    }
    auto next = [&](uint32_t p) {
        auto it = ptrs.upper_bound(p);
        return it == ptrs.end() ? p : *it;
    };

    Out o;
    o.U32(0xC), o.U32(0), o.U32(0);
    for (uint32_t p = 0xC;; p += 16) {
        if (!b.In(p, 16)) return false;
        for (int k = 0; k < 4; k++) o.U32(b.U32(p + 4 * k));
        if (b.U32(p) == 0xFFFFFFFF) break;
    }
    std::vector<std::map<uint32_t, uint32_t>> node_ptr(g.nodes);
    for (uint32_t i = 0; i < g.nodes; i++) {
        if (uint32_t state = word(i, 0)) node_ptr[i][0x00] = o.Put(Swap32(gc, state, next(state)));
        auto found = g.meshes.find(i);
        if (found == g.meshes.end()) continue;
        const Mesh& me = found->second;
        // a GameCube slot -> the Xbox palette entry, the first when two share a matrix
        std::map<uint32_t, uint32_t> slot_of;
        for (auto it = slots[i].rbegin(); it != slots[i].rend(); ++it) slot_of[it->second] = it->first;
        const auto& chunks = me.chunks;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (size_t k = 0; k < chunks.size(); k++)
            ranges.push_back({k == 0 ? 0u : chunks[k].start, uint32_t(chunks[k].start) + chunks[k].n});
        // runs of chunks, each up to one with strips, which index all of the run's vertices
        std::vector<GcVertex> verts;
        std::vector<uint32_t> counts(chunks.size());
        std::map<size_t, std::vector<uint16_t>> index_lists;
        for (size_t a = 0, k = 0; k < chunks.size(); k++) {
            if (me.strips[k].empty()) continue;
            size_t last = k;
            auto home = [&](const GcVertex& v) {
                for (size_t c = a; c <= last; c++)
                    if (ranges[c].first <= v.pos && v.pos < ranges[c].second) return c;
                return a;
            };
            std::vector<GcVertex> order;
            std::set<GcVertex> seen;
            for (auto& s : me.strips[last])
                for (auto& v : s)
                    if (seen.insert(v).second) order.push_back(v);
            std::stable_sort(order.begin(), order.end(), [&](auto& x, auto& y) { return home(x) < home(y); });
            std::map<GcVertex, uint16_t> index;
            for (auto& v : order) {
                index[v] = uint16_t(verts.size());
                verts.push_back(v);
                counts[home(v)]++;
            }
            std::vector<uint16_t> idx;
            for (auto& s : me.strips[last]) {
                if (!idx.empty()) {
                    uint16_t tail = idx.back();
                    idx.push_back(tail);
                    idx.push_back(index[s[0]]);
                    if (idx.size() % 2) idx.push_back(index[s[0]]);
                }
                for (auto& v : s) idx.push_back(index[v]);
            }
            index_lists[last] = std::move(idx);
            a = k + 1;
        }
        std::vector<uint32_t> starts;
        for (uint32_t c = 0, acc = 0; c < counts.size(); acc += counts[c], c++) starts.push_back(acc);
        // the vertex arrays
        o.Align(16);
        std::vector<uint32_t> pos, uv, nrm;
        for (auto& v : verts) {
            for (int c = 0; c < 3; c++)
                pos.push_back(std::bit_cast<uint32_t>(int16_t(b.U16(me.pos + v.pos * 6u + c * 2u)) / 1024.0f + g.off[c]));
            auto slot = slot_of.find(v.mtx / 3u);
            if (slot == slot_of.end()) return false;
            pos.push_back(0xFFFFFF00u | (0xE0u + 4u * slot->second));
            for (int c = 0; c < 2; c++) uv.push_back(std::bit_cast<uint32_t>(b.U16(me.uv + v.uv * 4u + c * 2u) / 1024.0f));
            uv.push_back(std::bit_cast<uint32_t>(1.0f));
            auto n = normals.find(v.nrm);
            nrm.push_back(n == normals.end() ? PackNormal({0, 1, 0}) : PackNormal(n->second));
            nrm.push_back(uint32_t(palettes[i][slot->second]));  // the vertex's matrix, which lights it
        }
        node_ptr[i][0x04] = o.Put(pos);
        node_ptr[i][0x08] = o.Put(uv);
        node_ptr[i][0x0C] = o.Put(nrm);
        uint32_t bounds = word(i, 0xA0);
        node_ptr[i][0xAC] = o.Put(Swap32(gc, bounds, bounds + 0x20));
        // the strip table, {index list, count} a chunk, then the lists
        std::vector<uint8_t> zeros(8 * chunks.size());
        uint32_t tab = o.Put(zeros);
        node_ptr[i][0xBC] = tab;
        o.Align(16);
        for (auto& [k, list] : index_lists) {
            uint32_t at = o.Put(list);
            o.Set32(tab + 8 * uint32_t(k), at);
            o.Set32(tab + 8 * uint32_t(k) + 4, uint32_t(list.size()));
        }
        o.Align(4);
        std::vector<uint8_t> ch;
        auto chunk = [&](uint16_t grp, uint8_t n, uint8_t kind, uint16_t idx, uint16_t start) {
            uint16_t w[5] = {grp, uint16_t(n | kind << 8), idx, start, 0xFFFF};
            ch.insert(ch.end(), reinterpret_cast<uint8_t*>(w), reinterpret_cast<uint8_t*>(w) + 10);
        };
        for (size_t k = 0; k < chunks.size(); k++)
            chunk(chunks[k].grp, uint8_t(counts[k]), chunks[k].kind, chunks[k].idx, uint16_t(starts[k]));
        chunk(0, 0, 0xFF, 0, 0);
        node_ptr[i][0x60] = o.Put(ch);
    }
    // the nodes: Xbox field <- GameCube field, as bytes are, or u32 / u16 swapped
    enum How { kRaw, kU32, kU16 };
    std::vector<std::array<uint32_t, 3>> fields = {{0x48, 0x3C, kRaw}, {0x4C, 0x40, kRaw}, {0x54, 0x48, kU16},
                                                   {0x5C, 0x50, kU32}, {0x70, 0x64, kU16}, {0xB8, 0xAC, kU32}};
    for (uint32_t k = 0; k < 0x18; k += 4) fields.push_back({0x90 + k, 0x84 + k, kU32});
    o.Align(4);
    for (uint32_t i = 0; i < g.nodes; i++) {
        uint32_t a = g.nodebase + i * kGcNode;
        std::array<uint8_t, kXbNode> rec{};
        for (auto& [xo, go, how] : fields) {
            const uint8_t* v = &gc[a + go];
            uint8_t* r = &rec[xo];
            if (how == kRaw) std::memcpy(r, v, 4);
            else if (how == kU32) r[0] = v[3], r[1] = v[2], r[2] = v[1], r[3] = v[0];
            else r[0] = v[1], r[1] = v[0], r[2] = v[3], r[3] = v[2];
        }
        for (auto& [xo, val] : node_ptr[i]) std::memcpy(&rec[xo], &val, 4);
        o.Put(rec.data(), rec.size());
    }
    // the header: GameCube words 0-8, three of 0, words 9-15; the GameCube's own flags off
    uint32_t hdr = uint32_t(o.b.size());
    for (uint32_t k = 0; k < 16; k++) {
        if (k == 9) o.U32(0), o.U32(0), o.U32(0);
        uint32_t w = b.U32(g.hdr + 4 * k);
        o.U32(k == 3 ? w & ~0x10030u : w);
    }
    uint32_t end = g.tab30;
    while (b.In(end, 0x30) && b.U32(end) != 0xFFFFFFFF) end += 0x30;
    if (!b.In(end, 0x30)) return false;
    uint32_t tab30 = o.Put(Swap32(gc, g.tab30, end + 0x30));
    o.Set32(4, hdr);
    o.Set32(8, tab30);
    out = std::move(o.b);
    return true;
}

// GameCube CMPR .gct -> Xbox DXT1 .xbt, the same blocks: CMPR's are in tiles of 2x2, colours big-endian, each row's
// pixels from the high bits
bool ConvertTexture(const std::vector<uint8_t>& gct, std::vector<uint8_t>& out) {
    Bytes b{gct, true};
    uint32_t w = b.U32(0), h = b.U32(4);
    if ((b.U32(0x10) & 0xFFFF) != 2 || !w || !h || w > 4096 || h > 4096) return false;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    out.assign(0x80 + size_t(bw) * bh * 8, 0);
    uint32_t head[7] = {w, h, w, h, 0, 0, 0xFFFFFFFF};
    std::memcpy(out.data(), head, sizeof(head));
    uint32_t o = 0x20;
    for (uint32_t ty = 0; ty < bh; ty += 2)
        for (uint32_t tx = 0; tx < bw; tx += 2)
            for (uint32_t sy = 0; sy < 2; sy++)
                for (uint32_t sx = 0; sx < 2; sx++, o += 8) {
                    if (!b.In(o, 8)) return false;
                    uint32_t bx = tx + sx, by = ty + sy;
                    if (bx >= bw || by >= bh) continue;
                    uint16_t c[2] = {b.U16(o), b.U16(o + 2)};
                    uint32_t idx = 0;
                    for (uint32_t r = 0; r < 4; r++)
                        for (uint32_t px = 0; px < 4; px++)
                            idx |= uint32_t((gct[o + 4 + r] >> (6 - 2 * px)) & 3) << (2 * (r * 4 + px));
                    uint8_t* d = &out[0x80 + (size_t(by) * bw + bx) * 8];
                    std::memcpy(d, c, 4);
                    std::memcpy(d + 4, &idx, 4);
                }
    return true;
}

// the beta's files by path, built once
std::map<std::string, std::vector<uint8_t>> g_files;
std::once_flag g_built;
std::atomic<bool> g_ready = false;

void Build() {
    const std::string& folder = game::Opt().ts4_folder;
    if (folder.empty()) return;
    fs::path root = fs::u8path(folder);
    fs::path models = root / "assets" / "ts" / "models";
    std::vector<uint8_t> gc;
    if (!ReadFile(models / "chrs" / "cortezingame" / "gamecube" / "output" / "cortezingame.gcr", gc)) {
        LOGR("beta cortez: no cortezingame.gcr in %s", folder.c_str());
        return;
    }
    Bytes b{gc, true};
    Model m;
    if (!ReadGc(b, m)) {
        LOGR("beta cortez: cortezingame.gcr not read");
        return;
    }
    std::set<uint32_t> wanted = NormalIndices(m);
    Normals normals = NormalTable(models, wanted);
    FaceNormals(b, m, wanted, normals);
    std::vector<uint8_t> xbr;
    if (!Convert(gc, normals, xbr)) {
        LOGR("beta cortez: cortezingame.gcr not converted");
        return;
    }
    // its textures, by the ids in its texture table
    for (uint32_t p = 0xC; b.In(p, 16) && b.U32(p) != 0xFFFFFFFF; p += 16) {
        char id[16];
        std::snprintf(id, sizeof(id), "%06x", b.U32(p));
        std::vector<uint8_t> gct, xbt;
        bool read = false;
        // the model's own folder first: the demo's textures folder has some of them empty
        for (fs::path dir : {models / "chrs" / "cortezingame" / "textures", root / "textures", root / "chr" / "textures"})
            if ((read = ReadFile(dir / (std::string(id) + ".gct"), gct) && !gct.empty())) break;
        if (!read || !ConvertTexture(gct, xbt)) {
            LOGR("beta cortez: texture %s not found or not CMPR", id);
            return;
        }
        g_files[std::string("textures/") + id + ".xbt"] = std::move(xbt);
    }
    LOGR("beta cortez: cortezingame.xbr built, %zu bytes, %zu textures", xbr.size(), g_files.size());
    g_files[kBetaPath] = std::move(xbr);
    g_ready = true;
}

bool Built() {
    std::call_once(g_built, Build);
    return g_ready;
}

bool Cutscene() {
    return std::find(std::begin(kCutsceneMaps), std::end(kCutsceneMaps), int32_t(kernel::R32(kMap))) !=
           std::end(kCutsceneMaps);
}

// the cutscene model, out of its pak when first asked for
std::once_flag g_cs_read;
std::vector<uint8_t> g_cs;

const std::vector<uint8_t>* CutsceneModel() {
    std::call_once(g_cs_read, [] {
        std::vector<uint8_t> pak;
        if (!kernel::ReadGameFile(kCsPak, pak) || pak.size() < 16 || std::memcmp(pak.data(), "P5CK", 4)) {
            LOGR("beta cortez: no %s", kCsPak);
            return;
        }
        Bytes pb{pak, false};
        for (uint32_t e = pb.U32(4), end = e + pb.U32(8); e + 16 <= end && pb.In(e, 16); e += 16) {
            if (pb.U32(e) != Crc32(kCsPath) || !pb.In(pb.U32(e + 4), pb.U32(e + 8))) continue;
            g_cs.assign(pak.begin() + pb.U32(e + 4), pak.begin() + pb.U32(e + 4) + pb.U32(e + 8));
            LOGR("beta cortez: %s, %zu bytes", kCsName, g_cs.size());
            return;
        }
        LOGR("beta cortez: no %s in %s", kCsName, kCsPak);
    });
    return g_cs.empty() ? nullptr : &g_cs;
}

}  // namespace

namespace game {

const std::vector<uint8_t>* BetaSkylineFile(const std::string& path);                       // game/beta_skyline.cpp
bool BetaSkylineNames(const std::string& name, uint32_t& name_at, uint32_t& file_at);  // game/beta_skyline.cpp

// a file of the beta Cortez the paks lack (path as the game asks for it, after any "host0:"), built when first asked
const std::vector<uint8_t>* BetaCortezFile(const std::string& path) {
    if (path == kCsPath) return CutsceneModel();
    // the textures only once the model is built
    if (path == kBetaPath ? !Built() : !g_ready) return nullptr;
    auto it = g_files.find(path);
    return it == g_files.end() ? nullptr : &it->second;
}

}  // namespace game

// sprintf(buffer, format, ...): a model's path, the final Cortez's given the beta's when it is chosen, in every mode
// (Story, Arcade, MapMaker, the character select): in the cutscenes the beta's cutscene model, elsewhere its in-game one;
// and Breaking and Entering's skyline towers the early ones (game/beta_skyline.cpp)
X86_EXTERN_FUNC(__imp__sub_003C85D4);
X86_FUNC(sub_003C85D4) {
    // the first model path formatted (the menus' characters) starts building the beta Cortez on a thread of its own:
    // built while the level loads it took a second of frames at 35 fps
    if (game::Opt().cortez_model == 1) {
        static std::once_flag started;
        std::call_once(started, [] { std::thread([] { Built(); }).detach(); });
    }
    if (kernel::R32(ctx.esp + 8) == kModelPath && game::Opt().skyline_model == 1) {
        uint32_t name_at, file_at;
        if (game::BetaSkylineNames(kernel::ReadCString(kernel::R32(ctx.esp + 12), 128), name_at, file_at)) {
            kernel::W32(ctx.esp + 12, name_at);
            kernel::W32(ctx.esp + 16, file_at);
        }
    }
    if (kernel::R32(ctx.esp + 8) == kModelPath && game::Opt().cortez_model == 1) {
        bool cutscene = Cutscene();
        std::string name = kernel::ReadCString(kernel::R32(ctx.esp + 12), 128);
        if (std::find(std::begin(kFinal), std::end(kFinal), name) != std::end(kFinal) &&
            (cutscene ? CutsceneModel() != nullptr : Built())) {
            static uint32_t names = 0;  // the in-game names, then the cutscene ones
            if (!names && (names = mem::AllocSystem(128))) {
                std::memcpy(kernel::HostPtr(names), kBetaName, sizeof(kBetaName));
                std::memcpy(kernel::HostPtr(names + 32), kBetaFile, sizeof(kBetaFile));
                std::memcpy(kernel::HostPtr(names + 64), kCsName, sizeof(kCsName));
                std::memcpy(kernel::HostPtr(names + 96), kCsFile, sizeof(kCsFile));
            }
            if (names) {
                uint32_t at = names + (cutscene ? 64 : 0);
                kernel::W32(ctx.esp + 12, at);
                kernel::W32(ctx.esp + 16, at + 32);
                static bool logged[2] = {};
                if (!logged[cutscene]) LOGR("beta cortez: %s loaded as %s", name.c_str(), cutscene ? kCsName : kBetaName);
                logged[cutscene] = true;
            }
        }
    }
    __imp__sub_003C85D4(ctx, base);
}

// Characters in Arcade and MapMaker are streamed: the model and then each texture opened (0x62E20, which falls back on
// the disc when no pak has the file) and read in the background (0x29370 into a buffer the size 0x62CA0 tells). The
// beta's files and the mods' (common/mods.h) are on neither: they get handles of their own, out of the stream table's 200 (so its poll 0x294B0 says
// done and its close 0x29400 does nothing), and their reads are done at once.
namespace {

constexpr uint32_t kStreamHandle = 0x5EED0000 | 0xF000;  // and an index into g_streams
std::mutex g_stream_lock;
std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> g_streams;  // a handle's file and offset into it

// a file the game asks for (name) as the beta's files are named: no "host0:", textures .xbt
std::string FilePath(uint32_t name) {
    std::string path = kernel::ReadCString(name, 256);
    if (size_t at = path.find("host0:"); at != std::string::npos) path = path.substr(at + 6);
    if (path.size() >= 4 && !path.compare(path.size() - 4, 4, ".raw")) path.replace(path.size() - 4, 4, ".xbt");
    return path;
}

// once a file of the beta's: how it was served
void LogServed(const std::string& path, const char* how) {
    static std::mutex lock;
    static std::set<std::string> logged;
    std::lock_guard guard(lock);
    if (logged.insert(path + how).second) LOGR("beta files: %s %s", path.c_str(), how);
}

}  // namespace

namespace {

// a copy of a file in memory, as a pak loaded whole gives it; 0 when there is no room. A texture is drawn from where it
// lies, by its physical address: it goes in contiguous memory, as the paks are loaded (in system memory the GPU read
// other memory, and the ghost girl's served texture drew black)
uint32_t InMemory(const std::vector<uint8_t>& file, bool texture = false) {
    uint32_t at = texture ? mem::AllocContiguous(uint32_t(file.size()), 0, 0xFFFFFFFF, 128)
                          : mem::AllocSystem(uint32_t(file.size()), 128);
    if (at) std::memcpy(kernel::HostPtr(at), file.data(), file.size());
    return at;
}

// a stream's handle on a file of ours
uint32_t StreamOf(const std::vector<uint8_t>* file, uint32_t offset) {
    std::lock_guard lock(g_stream_lock);
    auto it = std::find(g_streams.begin(), g_streams.end(), std::pair{file, offset});
    if (it == g_streams.end()) it = g_streams.insert(g_streams.end(), {file, offset});
    return kStreamHandle + uint32_t(it - g_streams.begin());
}

}  // namespace

// MappedFile(name, ...): a file in a pak loaded whole (Story's levels), where it is in memory, 0 when no such pak has
// it; a mod's in place of the pak's, the beta's when the paks lack it, each a copy of its own (models are fixed up
// where they lie, so each load has its own)
X86_EXTERN_FUNC(__imp__sub_000624C0);
X86_FUNC(sub_000624C0) {
    uint32_t name = kernel::R32(ctx.esp + 4);
    if (auto mod = game::mods::File(kernel::ReadCString(name, 256))) {
        std::string path = FilePath(name);
        if (uint32_t at = InMemory(*mod, path.size() >= 4 && !path.compare(path.size() - 4, 4, ".xbt"))) {
            game::mods::Read(path, at);
            kernel::Return(ctx, at, 0);
            return;
        }
    }
    __imp__sub_000624C0(ctx, base);
    if (ctx.eax) return;
    std::string path = FilePath(name);
    auto file = game::BetaSkylineFile(path);
    if (!file) file = game::BetaCortezFile(path);
    if (!file) return;
    if (uint32_t at = InMemory(*file)) {
        ctx.eax = at;
        LogServed(path, "served in memory");
    }
}

namespace {

// a cutscene's characters (csinfo's CHR lines, 0x6FD84 -> LoadCsModel 0x97FC0) are taken from the scene's pak in
// place, with no way to read them otherwise: a mod's goes in contiguous memory (its vertices and textures are drawn
// from there), one copy per file, filled again each load since the model is fixed up where it lies. One scene plays
// at a time ([0x6F73B4]), so the copies of the scene before are let go when another's are asked for.
constexpr uint32_t kCsModelLoad = 0x97FE1;  // return address of LoadCsModel's call
std::mutex g_cs_lock;
std::map<std::string, std::pair<uint32_t, uint32_t>> g_cs_copies;  // path -> address, size
std::string g_cs_scene;

uint32_t CsModelCopy(const std::string& path, const std::vector<uint8_t>& file) {
    std::lock_guard lock(g_cs_lock);
    std::string scene = path.substr(0, path.rfind('/'));
    if (scene != g_cs_scene) {
        for (auto& [p, copy] : g_cs_copies) mem::FreeContiguous(copy.first);
        g_cs_copies.clear();
        g_cs_scene = scene;
    }
    auto& copy = g_cs_copies[path];
    if (copy.first && copy.second < file.size()) mem::FreeContiguous(copy.first), copy = {};
    if (!copy.first) copy = {mem::AllocContiguous(uint32_t(file.size()), 0, 0xFFFFFFFF, 128), uint32_t(file.size())};
    if (copy.first) std::memcpy(kernel::HostPtr(copy.first), file.data(), file.size());
    LOGR("mods: cutscene model %s at %08X (%zu bytes)", path.c_str(), copy.first, file.size());
    return copy.first;
}

}  // namespace

// InLoadedPak(name, &size): where a file of the level's pak loaded whole lies, 0 when it has none (Story's models
// and textures, through 0x62F50). A file a mod gives is one the pak has not: the caller then reads it as Arcade does
// (FileSize/ReadFile, served there) into memory the game allocates, which its drawing takes vertices from; a copy of
// ours elsewhere left Cortez unseen. A cutscene's character has no such way: it gets a copy of the mod's (above).
X86_EXTERN_FUNC(__imp__sub_00062C30);
X86_FUNC(sub_00062C30) {
    uint32_t name = kernel::R32(ctx.esp + 4), from = kernel::R32(ctx.esp);
    __imp__sub_00062C30(ctx, base);
    if (!ctx.eax) return;
    std::string path = kernel::ReadCString(name, 256);
    if (auto mod = game::mods::File(path)) {
        if (from != kCsModelLoad) ctx.eax = 0;
        else if (uint32_t at = CsModelCopy(path, *mod)) ctx.eax = at;
    }
}

// OpenStream(name, offset): a handle on the file in a pak or on the disc, -1 when neither has it; a mod's in place of
// those, the beta's when neither has it
X86_EXTERN_FUNC(__imp__sub_00062E20);
X86_FUNC(sub_00062E20) {
    uint32_t name = kernel::R32(ctx.esp + 4), offset = kernel::R32(ctx.esp + 8);
    if (auto mod = game::mods::File(kernel::ReadCString(name, 256))) {
        kernel::Return(ctx, StreamOf(mod, offset), 0);
        return;
    }
    __imp__sub_00062E20(ctx, base);
    if (ctx.eax != 0xFFFFFFFF) return;
    std::string path = FilePath(name);
    auto file = game::BetaSkylineFile(path);
    if (!file) file = game::BetaCortezFile(path);
    if (!file) return;
    ctx.eax = StreamOf(file, offset);
    LogServed(path, "streamed");
}

// ReadStream(handle, buffer, offset, size, priority): a read started, done at once on the beta's files
X86_EXTERN_FUNC(__imp__sub_00029370);
X86_FUNC(sub_00029370) {
    uint32_t handle = kernel::R32(ctx.esp + 4);
    if (handle - kStreamHandle < 0x1000) {
        std::lock_guard lock(g_stream_lock);
        if (handle - kStreamHandle < g_streams.size()) {
            auto [file, at] = g_streams[handle - kStreamHandle];
            uint64_t from = uint64_t(at) + kernel::R32(ctx.esp + 12), size = kernel::R32(ctx.esp + 16);
            uint32_t buffer = kernel::R32(ctx.esp + 8);
            if (from < file->size()) std::memcpy(kernel::HostPtr(buffer), file->data() + from, size_t(std::min<uint64_t>(size, file->size() - from)));
            kernel::Return(ctx, 1, 0);
            return;
        }
    }
    __imp__sub_00029370(ctx, base);
}
