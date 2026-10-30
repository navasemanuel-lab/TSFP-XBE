#include "gpu/interp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpu::interp {

namespace {

constexpr double kTurnCos = 0.92387953;  // cos(45 degrees / 2): a quaternion turned more than 45 degrees
constexpr double kMove = 0.5;            // of the distance
// The nearest a distance is taken as (the game's units, a character some 1.7 tall): half the distance of a wall
// beside the camera or of the hands is a step, not a cut, and that piece left at the newer frame while the rest
// blended was the saw along the wall's edge and the cracks between a body's pieces
constexpr double kNear = 2.0;
constexpr double kAffineEps = 1e-3;
constexpr double kZoomCut = 1.15;  // the projection's zoom changed by more in a frame: another shot

using M4 = double[4][4];
using M3 = double[3][3];

void Load(const std::array<float, 4>* r, M4 m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = r[i][j];
}

void Store(const M4 m, std::array<float, 4>* r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i][j] = float(m[i][j]);
}

bool Affine(const M4 m) {
    return std::fabs(m[3][0]) < kAffineEps && std::fabs(m[3][1]) < kAffineEps && std::fabs(m[3][2]) < kAffineEps &&
           std::fabs(m[3][3] - 1) < kAffineEps;
}

void Mul(const M4 a, const M4 b, M4 out) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j];
            out[i][j] = s;
        }
}

// Gauss-Jordan with partial pivoting; false when singular
bool Invert(const M4 m, M4 out) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i][j];
            a[i][j + 4] = i == j;
        }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int j = 0; j < 8; j++) std::swap(a[p][j], a[c][j]);
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            if (f != 0)
                for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = a[i][j + 4];
    return true;
}

double Det3(const M3 m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// the rotation of l by polar decomposition (Newton's iteration), false for a mirrored or flat matrix
bool Rotation(const M3 l, M3 r) {
    if (Det3(l) <= 1e-12) return false;
    std::memcpy(r, l, sizeof(M3));
    for (int it = 0; it < 12; it++) {
        double d = Det3(r);
        if (std::fabs(d) < 1e-12) return false;
        // the inverse's transpose: the cofactors over the determinant
        M3 c;
        c[0][0] = (r[1][1] * r[2][2] - r[1][2] * r[2][1]) / d;
        c[0][1] = -(r[1][0] * r[2][2] - r[1][2] * r[2][0]) / d;
        c[0][2] = (r[1][0] * r[2][1] - r[1][1] * r[2][0]) / d;
        c[1][0] = -(r[0][1] * r[2][2] - r[0][2] * r[2][1]) / d;
        c[1][1] = (r[0][0] * r[2][2] - r[0][2] * r[2][0]) / d;
        c[1][2] = -(r[0][0] * r[2][1] - r[0][1] * r[2][0]) / d;
        c[2][0] = (r[0][1] * r[1][2] - r[0][2] * r[1][1]) / d;
        c[2][1] = -(r[0][0] * r[1][2] - r[0][2] * r[1][0]) / d;
        c[2][2] = (r[0][0] * r[1][1] - r[0][1] * r[1][0]) / d;
        double change = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double n = 0.5 * (r[i][j] + c[i][j]);
                change += std::fabs(n - r[i][j]);
                r[i][j] = n;
            }
        if (change < 1e-10) break;
    }
    return true;
}

void ToQuat(const M3 m, double q[4]) {
    double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        double s = std::sqrt(tr + 1) * 2;
        q[3] = s / 4;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = s / 4;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = s / 4;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = s / 4;
    }
}

enum class Result { Blended, Cut };

// term by term: a point two draws share (the edge between two bones' pieces of a face, two pieces of a wall, each
// with its matrix) is where both put it in either frame, and so in every frame between, as the line between those
// two places. Taken apart and turned in an arc instead, as lmplied's and ours were, each matrix carried the point on
// an arc of its own about its own origin: the seams opened (the face's cracks, the saw along a wall's edge). A
// frame turns a few degrees, so the line is the arc to a hair.
void Lerp(const M4 a, const M4 b, double t, M4 out) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = a[i][j] + (b[i][j] - a[i][j]) * t;
}

// two affine matrices blended at t into out, or a cut: turned more than 45 degrees or moved more than half its
// distance, or mirrored in one frame and not the other
Result BlendAffine(const M4 a, const M4 b, double t, M4 out) {
    M3 la, lb, ra, rb;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            la[i][j] = a[i][j];
            lb[i][j] = b[i][j];
        }
    // a mirrored matrix (one side of a face, a view with its z turned) is minus a rotation times the rest
    bool mirrored = Det3(la) < 0;
    if (mirrored != (Det3(lb) < 0)) return Result::Cut;
    if (mirrored)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                la[i][j] = -la[i][j];
                lb[i][j] = -lb[i][j];
            }
    double ta[3] = {a[0][3], a[1][3], a[2][3]}, tb[3] = {b[0][3], b[1][3], b[2][3]};
    double da = std::sqrt(ta[0] * ta[0] + ta[1] * ta[1] + ta[2] * ta[2]);
    double db = std::sqrt(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
    double dd = std::sqrt((tb[0] - ta[0]) * (tb[0] - ta[0]) + (tb[1] - ta[1]) * (tb[1] - ta[1]) +
                          (tb[2] - ta[2]) * (tb[2] - ta[2]));
    if (dd > kMove * std::max({da, db, kNear})) return Result::Cut;
    if (Rotation(la, ra) && Rotation(lb, rb)) {
        double qa[4], qb[4];
        ToQuat(ra, qa);
        ToQuat(rb, qb);
        double d = std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
        if (d < kTurnCos) return Result::Cut;
    }
    Lerp(a, b, t, out);
    out[3][0] = out[3][1] = out[3][2] = 0;
    out[3][3] = 1;
    return Result::Blended;
}

// the affine matrix a product is (the projection's inverse times it) into out, false when it is none: the
// projection is not the one the title multiplied in. Its last row may be off by what a float product of a far
// translation gets wrong, as lmplied's unproject allows, and is then made exact; held to 1e-3 instead, every
// distant piece was left at the newer frame while the nearer ones blended
bool Unproject(const M4 inv, const M4 g, M4 out) {
    Mul(inv, g, out);
    double scale = 1;
    for (int i = 0; i < 3; i++) scale = std::max(scale, std::fabs(out[i][3]));
    if (std::fabs(out[3][0]) > 1e-3 || std::fabs(out[3][1]) > 1e-3 || std::fabs(out[3][2]) > 1e-3 ||
        std::fabs(out[3][3] - 1) > 1e-4 * scale)
        return false;
    out[3][0] = out[3][1] = out[3][2] = 0;
    out[3][3] = 1;
    return true;
}

// a group's four registers, FNV-1a
uint64_t Hash(const std::array<float, 4>* g) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < 4 * sizeof(*g); i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool Same(const std::array<float, 4>* a, const std::array<float, 4>* b) {
    return std::memcmp(a, b, 4 * sizeof(std::array<float, 4>)) == 0;
}

}  // namespace

bool Perspective(const float r[4][4]) {
    auto zero = [](float v) { return std::fabs(v) < 1e-4f; };
    return zero(r[0][1]) && zero(r[0][3]) && zero(r[1][0]) && zero(r[1][3]) && zero(r[2][0]) && zero(r[2][1]) &&
           zero(r[3][0]) && zero(r[3][1]) && zero(r[3][3]) && std::fabs(std::fabs(r[3][2]) - 1) < 1e-3f &&
           !zero(r[0][0]) && !zero(r[1][1]);
}

Outcome Blend(const Regs& older, const Regs& newer, float t, Regs& out, uint32_t groups) {
    Outcome o;
    out = newer;
    float pa[4][4], pb[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            pa[i][j] = older[i][j];
            pb[i][j] = newer[i][j];
        }
    o.perspective = Perspective(pb);
    // the projection: term by term between two of a kind
    M4 projection_a, projection_b, projection_t;
    Load(&older[0], projection_a);
    Load(&newer[0], projection_b);
    std::memcpy(projection_t, projection_b, sizeof(M4));
    // the zoom, the length of the projection's first row: a camera going on zooms a few percent a frame, a shot
    // cut to another (the same place nearer, someone else) at once, and blended its frame between was a third shot
    auto zoom = [](const float m[4][4]) { return std::sqrt(double(m[0][0]) * m[0][0] + double(m[0][1]) * m[0][1] +
                                                           double(m[0][2]) * m[0][2]); };
    if (!Same(&older[0], &newer[0])) {
        o.moved = true;
        double za = zoom(pa), zb = zoom(pb);
        bool zoomed = za > 0 && zb > 0 && std::max(za, zb) > kZoomCut * std::min(za, zb);
        if (Perspective(pa) == o.perspective && (pa[3][2] > 0) == (pb[3][2] > 0) && !zoomed) {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    projection_t[i][j] = projection_a[i][j] + (projection_b[i][j] - projection_a[i][j]) * t;
            Store(projection_t, &out[0]);
        } else {
            o.cut = true;
        }
    }
    M4 inv_a, inv_b;
    bool have_inv = false, tried_inv = false;
    for (uint32_t g = 1; g < kGroups; g++) {
        if (!((groups >> g) & 1)) continue;
        const std::array<float, 4>* ga = &older[g * 4];
        const std::array<float, 4>* gb = &newer[g * 4];
        if (Same(ga, gb) && Same(&older[0], &newer[0])) continue;
        M4 a, b, blended;
        Load(ga, a);
        Load(gb, b);
        bool affine_a = Affine(a), affine_b = Affine(b);
        if (affine_a && affine_b) {
            if (Same(ga, gb)) continue;
            o.moved = true;
            Result r = BlendAffine(a, b, t, blended);
            if (r == Result::Cut) o.cut = true;
            else Store(blended, &out[g * 4]);
            continue;
        }
        if (affine_a != affine_b) {
            o.moved = o.cut = true;
            continue;
        }
        // the projection times an affine matrix: taken apart with each frame's projection
        if (!tried_inv) {
            tried_inv = true;
            have_inv = Invert(projection_a, inv_a) && Invert(projection_b, inv_b);
        }
        o.moved = true;
        if (!have_inv) continue;
        M4 ma, mb;
        if (!Unproject(inv_a, a, ma) || !Unproject(inv_b, b, mb)) {
            o.kept = true;
            continue;
        }
        Result r = BlendAffine(ma, mb, t, blended);
        if (r == Result::Cut) {
            o.cut = true;
            continue;
        }
        M4 product;
        Mul(projection_t, blended, product);
        Store(product, &out[g * 4]);
    }
    // a draw with a group that jumped is drawn whole as the newer frame has it, as lmplied's: its other groups
    // blended beside that one's newer values put its parts in two frames at once
    if (o.cut) out = newer;
    return o;
}

Outcome BlendBones(const Bones& older, const Bones& newer, float t, Bones& out, bool by_name) {
    Outcome o;
    out = newer;
    // Bones paired by their names, what moved is this draw's own: when most of them jumped (a character put
    // somewhere else, the camera cut) the draw is drawn as the newer frame has it, blended its bones crossed the
    // screen and the pieces flew apart. A few that jumped (a fast swing) are blended with the rest, a piece left at
    // the newer frame beside its blended neighbours a crack.
    uint32_t moved = 0, cut = 0;
    for (uint32_t g = 0; g < kBoneGroups; g++) {
        const std::array<float, 4>* ga = &older[g * 4];
        const std::array<float, 4>* gb = &newer[g * 4];
        if (Same(ga, gb)) continue;
        M4 a, b, blended;
        Load(ga, a);
        Load(gb, b);
        // registers past the draw's bones hold what was there before: other data, kept
        if (!Affine(a) || !Affine(b)) continue;
        moved++;
        Result r = BlendAffine(a, b, t, blended);
        if (r == Result::Cut) {
            cut++;
            Lerp(a, b, t, blended);
        }
        Store(blended, &out[g * 4]);
    }
    o.moved = moved > 0;
    o.cut = cut * 2 > moved;
    if (o.cut && by_name) out = newer;
    return o;
}

void Matcher::NextFrame() {
    for (uint32_t g = 1; g < kGroups; g++) {
        have_step_[g] = false;
        const Seen* most = nullptr;
        for (const auto& [h, seen] : seen_[g])
            if (!most || seen.count > most->count) most = &seen;
        M4 a, b, inv, step;
        if (most && most->count >= 4) {
            Load(most->older.data(), a);
            Load(most->newer.data(), b);
            if (Invert(b, inv)) {
                Mul(a, inv, step);
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++) step_[g][i][j] = step[i][j];
                have_step_[g] = true;
            }
        }
        seen_[g].clear();
    }
    older_.swap(newer_);
    newer_.clear();
    older_bones_.swap(newer_bones_);
    newer_bones_.clear();
    taken_.assign(older_.size(), 0);
    for (uint32_t t = 0; t < kTiers; t++) {
        older_by_key_[t].clear();
        for (uint32_t i = 0; i < older_.size(); i++) older_by_key_[t][older_[i].keys[t]].draws.push_back(i);
    }
}

// how far an older draw's matrices were placed from where the camera's step puts the newer one's: the translations
// of the groups (their origins after the projection), straight when there's no step
double Matcher::Apart(uint32_t i, const Regs& newer, uint32_t groups, const Bones* bones) const {
    const Regs& older = older_[i].regs;
    double d = 0;
    // a skinned draw's matrices are its bones, the registers above only its projection: two pieces of one character
    // told apart by where their bones were, a piece paired with another's bones tearing from its neighbours
    if (bones && older_[i].bones >= 0) {
        const Bones& ob = older_bones_[size_t(older_[i].bones)];
        for (uint32_t g = 0; g < kBoneGroups; g++) {
            if (ob[g * 4 + 3] != std::array<float, 4>{0, 0, 0, 1} || (*bones)[g * 4 + 3] != std::array<float, 4>{0, 0, 0, 1})
                continue;
            for (uint32_t r = 0; r < 3; r++) {
                double v = double(ob[g * 4 + r][3]) - double((*bones)[g * 4 + r][3]);
                d += v * v;
            }
        }
    }
    for (uint32_t g = 1; g < kGroups; g++) {
        if (!((groups >> g) & 1)) continue;
        double at[4];
        for (uint32_t r = 0; r < 4; r++) {
            at[r] = newer[g * 4 + r][3];
            if (!have_step_[g]) continue;
            at[r] = 0;
            for (uint32_t k = 0; k < 4; k++) at[r] += step_[g][r][k] * double(newer[g * 4 + k][3]);
        }
        for (uint32_t r = 0; r < 4; r++) {
            double v = double(older[g * 4 + r][3]) - at[r];
            d += v * v;
        }
    }
    return d;
}

int32_t Matcher::Match(const Keys& keys, const Regs& regs, const Bones* bones, uint32_t groups) {
    constexpr uint32_t kLooked = 32;  // candidates weighed at most, of a key many draws share
    newer_.push_back({keys, regs});
    newer_.back().groups = groups;
    if (bones) {
        newer_.back().bones = int32_t(newer_bones_.size());
        newer_bones_.push_back(*bones);
    }
    last_tier_ = kTiers;
    for (uint32_t t = 0; t < kTiers; t++) {
        auto it = older_by_key_[t].find(keys[t]);
        if (it == older_by_key_[t].end()) continue;
        Run& run = it->second;
        while (run.next < run.draws.size() && taken_[run.draws[run.next]]) run.next++;
        if (run.next == run.draws.size()) continue;
        uint32_t best = run.next;
        double best_d = Apart(run.draws[best], regs, groups, bones);
        for (uint32_t j = run.next + 1, looked = 1; j < run.draws.size() && looked < kLooked && best_d > 0; j++) {
            if (taken_[run.draws[j]]) continue;
            looked++;
            double d = Apart(run.draws[j], regs, groups, bones);
            if (d < best_d) best = j, best_d = d;
        }
        uint32_t i = run.draws[best];
        taken_[i] = 1;
        if (best == run.next) run.next++;
        last_tier_ = t;
        // the exact matches' matrices counted, for the next frame's step
        if (t == 0)
            for (uint32_t g = 1; g < kGroups; g++) {
                const Regs& o = older_[i].regs;
                if (!((groups >> g) & 1) || std::memcmp(&o[g * 4], &regs[g * 4], 4 * sizeof(o[0])) == 0) continue;
                Seen& seen = seen_[g][Hash(&regs[g * 4])];
                if (seen.count++ == 0) {
                    std::memcpy(seen.older.data(), &o[g * 4], sizeof(seen.older));
                    std::memcpy(seen.newer.data(), &regs[g * 4], sizeof(seen.newer));
                }
            }
        return int32_t(i);
    }
    return -1;
}

}  // namespace gpu::interp
