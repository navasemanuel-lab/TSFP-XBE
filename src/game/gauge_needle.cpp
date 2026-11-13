// The flamethrower's pressure gauge: the model has the needle (six vertices) and the dial's face at exactly one height,
// the needle first in the draw (177 indices, 126 vertices), so the two tie and the dial, drawn later over a less-equal
// depth test, paints over the needle or cuts it into a comb as the gun moves. The needle is lifted off the face, still
// under the gauge's rim (0.0295), the first time the model is drawn; a mended model no longer matches.
// The SBP90's blue sight line is the same: a four vertex strip, added and writing depth, in the plane (z -0.0644) of a
// face of the gun's body drawn just before it; it is lifted toward the player (his view runs along +z).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace {

constexpr uint32_t kIndices = 177, kVertices = 126, kFirstNeedle = 35, kNeedle = 6;
constexpr float kFace = 0.0266f, kLift = 0.0004f;

float ReadFloat(uint32_t at) {
    uint32_t v = kernel::R32(at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void WriteFloat(uint32_t at, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    kernel::W32(at, v);
}

void MendNeedle(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    if (count != kIndices) return;
    uint32_t first = 0xFFFF, last = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = kernel::R16(indices + i * 2);
        first = std::min(first, v);
        last = std::max(last, v);
    }
    if (last - first + 1 != kVertices) return;
    // the first needle vertex, by where it is on the gauge
    uint32_t at = positions + (first + kFirstNeedle) * stride;
    if (std::fabs(ReadFloat(at) - 0.0374f) > 0.0002f || std::fabs(ReadFloat(at + 4) + 0.0507f) > 0.0002f) return;
    for (uint32_t i = 0; i < kNeedle; i++) {
        uint32_t z = at + i * stride + 8;
        float now = ReadFloat(z);
        if (std::fabs(now - kFace) > 0.0001f) return;  // mended already
        WriteFloat(z, now + kLift);
    }
}

// the SBP90's sight line: four indices, four vertices, all at z -0.0644, the strip 0.0323 wide
void MendSight(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    constexpr float kPlane = -0.0644f, kSightLift = 0.0004f;
    if (count != 4) return;
    uint32_t first = 0xFFFF, last = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = kernel::R16(indices + i * 2);
        first = std::min(first, v);
        last = std::max(last, v);
    }
    if (last - first != 3) return;
    float x0 = 1e9f, x1 = -1e9f;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t at = positions + (first + i) * stride;
        if (std::fabs(ReadFloat(at + 8) - kPlane) > 0.0001f) return;  // mended already, or another strip
        x0 = std::min(x0, ReadFloat(at));
        x1 = std::max(x1, ReadFloat(at));
    }
    if (std::fabs((x1 - x0) - 0.0323f) > 0.0003f) return;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t z = positions + (first + i) * stride + 8;
        WriteFloat(z, ReadFloat(z) - kSightLift);
    }
}

// the vertices a draw uses, once each
std::vector<uint32_t> Used(uint32_t count, uint32_t indices) {
    std::vector<uint32_t> v(count);
    for (uint32_t i = 0; i < count; i++) v[i] = kernel::R16(indices + i * 2);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// the injector's ammo display: three added draws (13, 24 and 22 indices, no depth writes) on the face of the body, z
// -0.2081, the rear end, so they are lifted toward the player like the sight line
void MendDisplay(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    constexpr float kPlane = -0.2081f, kDisplayLift = 0.0004f;
    if (count != 13 && count != 24 && count != 22) return;
    auto used = Used(count, indices);
    for (uint32_t v : used) {
        uint32_t at = positions + v * stride;
        float x = ReadFloat(at), y = ReadFloat(at + 4);
        if (std::fabs(ReadFloat(at + 8) - kPlane) > 0.0001f) return;  // mended already, or another model
        if (x < -0.0075f || x > 0.0070f || y < 0.0835f || y > 0.0965f) return;
    }
    for (uint32_t v : used) {
        uint32_t z = positions + v * stride + 8;
        WriteFloat(z, ReadFloat(z) - kDisplayLift);
    }
}

// the flare gun's side plate: 34 indices, 24 vertices, twelve of them in the plane z 0.0132 of the body's face (the
// body's 170 indices come first and are drawn with the plate over it); the plate's vertices in that plane are lifted
// to the outside, +z, where both lie on the other side
void MendPlate(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    constexpr float kPlane = 0.0132f, kPlateLift = 0.0004f;
    if (count != 34) return;
    auto used = Used(count, indices);
    if (used.size() != 24) return;
    for (uint32_t v : used) {
        uint32_t at = positions + v * stride;
        float x = ReadFloat(at), y = ReadFloat(at + 4), z = ReadFloat(at + 8);
        if (x < -0.0025f || x > 0.0045f || y < 0.0124f || y > 0.0212f || z < -0.0146f || z > 0.0137f) return;
    }
    for (uint32_t v : used) {
        uint32_t z = positions + v * stride + 8;
        float now = ReadFloat(z);
        if (std::fabs(now - kPlane) < 0.0001f) WriteFloat(z, now + kPlateLift);
    }
}

// the flare gun's decal: a tilted quad of four vertices, blended and not writing depth, in the plane of a thin part of
// the gun drawn just before it (70 indices); lifted along the plane's normal, to the side away from that part
void MendDecal(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    constexpr float kLiftDecal = 0.0004f;
    constexpr float corner[4][3] = {{-0.0088f, -0.0476f, 0.0181f}, {-0.0098f, -0.0486f, -0.0171f},
                                    {0.0029f, 0.0149f, 0.0171f}, {0.0029f, 0.0149f, -0.0181f}};
    constexpr float n[3] = {0.98257f, -0.18458f, -0.02217f};
    if (count != 4) return;
    auto used = Used(count, indices);
    if (used.size() != 4) return;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t at = positions + used[k] * stride;
        for (int c = 0; c < 3; c++)
            if (std::fabs(ReadFloat(at + c * 4) - corner[k][c]) > 0.0003f) return;  // another quad, or mended already
    }
    for (uint32_t k = 0; k < 4; k++)
        for (int c = 0; c < 3; c++) {
            uint32_t at = positions + used[k] * stride + c * 4;
            WriteFloat(at, ReadFloat(at) + n[c] * kLiftDecal);
        }
}

// the Khallos Express's wall tapestries: 157 indices, 102 vertices, every one in a plane z +-3.157 of the carriage; the
// wall behind them (2152 indices drawn just before) is at +-3.160, 0.002 to 0.005 farther, too close for the depth
// test at a distance, so the tapestry flickers as the view turns. Moved 0.012 toward the middle of the carriage, where
// the player is
void MendTapestry(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    constexpr float kWall = 3.157f, kTapestryLift = 0.012f;
    if (count != 157) return;
    auto used = Used(count, indices);
    if (used.size() != 102) return;
    for (uint32_t v : used)
        if (std::fabs(std::fabs(ReadFloat(positions + v * stride + 8)) - kWall) > 0.0025f) return;  // mended already, or another
    for (uint32_t v : used) {
        uint32_t z = positions + v * stride + 8;
        float now = ReadFloat(z);
        WriteFloat(z, now > 0 ? now - kTapestryLift : now + kTapestryLift);
    }
}

void Mend(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) {
    MendTapestry(count, indices, positions, stride);
    MendDecal(count, indices, positions, stride);
    MendNeedle(count, indices, positions, stride);
    MendSight(count, indices, positions, stride);
    MendDisplay(count, indices, positions, stride);
    MendPlate(count, indices, positions, stride);
}

const bool g_set = [] {
    d3d::g_indexed_draw_hook = Mend;
    return true;
}();

}  // namespace
