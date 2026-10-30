// A mod's model adjusted (common/mods.h, Adjust): its vertices scaled, turned and moved about the middle of its box, as
// the settings panel's Weapon models page sets them. A .xbr keeps each pass's positions (16 bytes: x y z, a tag) and
// normals (8 bytes: 11:11:10 packed, a matrix) at file offsets in the pass words of its nodes (0xC8 bytes each, before
// the header the file's second word points to); their counts are the pass's chunk table's.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "common/mods.h"

namespace game {

namespace {

constexpr uint32_t kNode = 0xC8;

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

struct Arrays {
    uint32_t pos, nrm, count;
};

// each pass's positions and normals, once each
std::vector<Arrays> Find(const std::vector<uint8_t>& d) {
    std::vector<Arrays> out;
    uint32_t hdr = U32(d, 4), count = U32(d, hdr);
    if (!count || count > 1000 || hdr < count * kNode || hdr + 8 > d.size()) return out;
    uint32_t base = hdr - count * kNode;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = base + i * kNode;
        for (uint32_t p = 0; p < 3; p++) {
            uint32_t pos = U32(d, a + 0x18 * p + 4), nrm = U32(d, a + 0x18 * p + 12), chunks = U32(d, a + 0x60 + 4 * p);
            if (!pos || !chunks || !seen.insert(pos).second) continue;
            uint32_t n = 0;
            for (uint32_t c = chunks; c + 10 <= d.size(); c += 10) {
                if (d[c + 2] == 0 && d[c + 3] == 0xFF) break;
                n += d[c + 2];
            }
            if (!n || pos + 16ull * n > d.size() || (nrm && nrm + 8ull * n > d.size())) continue;
            out.push_back({pos, nrm, n});
        }
    }
    return out;
}

int Signed(uint32_t v, int bits) {
    v &= (1u << bits) - 1;
    return v >> (bits - 1) ? int(v) - (1 << bits) : int(v);
}

uint32_t Pack(float v, int bits) {
    int m = (1 << (bits - 1)) - 1;
    return uint32_t(int(std::lround(std::clamp(v, -1.0f, 1.0f) * float(m)))) & ((1u << bits) - 1);
}

}  // namespace

std::vector<uint8_t> AdjustModel(const std::vector<uint8_t>& original, const mods::Adjust& a,
                                 std::vector<std::pair<uint32_t, uint32_t>>* changed) {
    std::vector<uint8_t> d = original;
    std::vector<Arrays> arrays = Find(d);
    if (arrays.empty()) return d;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Arrays& r : arrays)
        for (uint32_t k = 0; k < r.count; k++)
            for (int c = 0; c < 3; c++) {
                float v;
                std::memcpy(&v, d.data() + r.pos + 16 * k + 4 * c, 4);
                lo[c] = std::min(lo[c], v);
                hi[c] = std::max(hi[c], v);
            }
    float mid[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    float s[3] = {a.scale * a.girth, a.scale * a.girth, a.scale * a.length};
    // turned by yaw about y, then pitch about x, then roll about z (degrees)
    const float r = 3.14159265f / 180.0f;
    float cy = std::cos(a.yaw * r), sy = std::sin(a.yaw * r), cp = std::cos(a.pitch * r), sp = std::sin(a.pitch * r),
          cr = std::cos(a.roll * r), sr = std::sin(a.roll * r);
    float rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    float rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    auto mul = [](const float (&p)[3][3], const float (&q)[3][3], float (&o)[3][3]) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) o[i][j] = p[i][0] * q[0][j] + p[i][1] * q[1][j] + p[i][2] * q[2][j];
    };
    float t[3][3], m[3][3];
    mul(rx, rz, t);
    mul(ry, t, m);
    float move[3] = {a.x, a.y, a.z};
    for (const Arrays& ar : arrays) {
        for (uint32_t k = 0; k < ar.count; k++) {
            float p[3], q[3];
            std::memcpy(p, d.data() + ar.pos + 16 * k, 12);
            for (int c = 0; c < 3; c++) p[c] = (p[c] - mid[c]) * s[c];
            for (int c = 0; c < 3; c++) q[c] = mid[c] + move[c] + m[c][0] * p[0] + m[c][1] * p[1] + m[c][2] * p[2];
            std::memcpy(d.data() + ar.pos + 16 * k, q, 12);
            if (!ar.nrm) continue;
            uint32_t w;
            std::memcpy(&w, d.data() + ar.nrm + 8 * k, 4);
            float n[3] = {Signed(w, 11) / 1023.0f / s[0], Signed(w >> 11, 11) / 1023.0f / s[1],
                          Signed(w >> 22, 10) / 511.0f / s[2]};
            float o[3], len = 0;
            for (int c = 0; c < 3; c++) {
                o[c] = m[c][0] * n[0] + m[c][1] * n[1] + m[c][2] * n[2];
                len += o[c] * o[c];
            }
            len = len > 0 ? std::sqrt(len) : 1.0f;
            w = Pack(o[0] / len, 11) | Pack(o[1] / len, 11) << 11 | Pack(o[2] / len, 10) << 22;
            std::memcpy(d.data() + ar.nrm + 8 * k, &w, 4);
        }
        if (changed) {
            changed->push_back({ar.pos, 16 * ar.count});
            if (ar.nrm) changed->push_back({ar.nrm, 8 * ar.count});
        }
    }
    return d;
}

}  // namespace game
