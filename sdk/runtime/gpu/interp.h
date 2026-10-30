// Frame interpolation's matching and blending: frames shown between the title's own, its logic still stepping 60
// times a second (gpu/d3d12.cpp draws them). A title of this era steps its world once a frame, so it cannot run faster
// without running fast; each frame is drawn again with the matrices in its vertex constants part of the way back
// toward the frame before, as the TimeSplitters Future Perfect recompilation does (lmplied/TSFPRecomp,
// hle_d3d8_interp.c, GPLv3; its registers and thresholds).
//
// The registers: four groups of four from the first, the projection, then 4x4 matrices, each either affine (a model
// view: last row 0 0 0 1) or the projection times one (Future Perfect uploads that product and draws most things
// with it alone). Each group of a draw is blended on its own, term by term (so a point two matrices share stays
// shared: interp.cpp, Lerp); a product is taken apart with the draw's projection, its affine part blended and the
// blended projection multiplied back in. A group that turned more than 45 degrees or moved more than half its distance (two units at
// the nearest) in a frame is a cut, and keeps the newer frame's values.
#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gpu::interp {

constexpr uint32_t kGroups = 4;
constexpr uint32_t kRegs = kGroups * 4;
using Regs = std::array<std::array<float, 4>, kRegs>;

// what blending a draw found
struct Outcome {
    bool perspective = false;  // the draw's projection is one: a 3D draw
    bool moved = false;        // a group differs between the frames
    bool cut = false;          // a group jumped
    bool kept = false;         // a product that would not come apart, kept at the newer frame
};

// the blended registers at t (0 the older frame, 1 the newer) into out, of the groups the draw reads (bit g of
// groups): the others hold what an earlier draw left, another object's matrices, kept as they are and never taken
// for a jump
Outcome Blend(const Regs& older, const Regs& newer, float t, Regs& out, uint32_t groups = 0xF);

// A skinned draw's bones: from c-32, 4x4 model views a program reads with a0 (Future Perfect's characters, hands and
// guns; the projection alone in the registers above). The game loads a batch's palette from c-32 (register 0x40,
// its matrix table's entry 0) and the program reads c[a0 + 96] with a0 from -32 up: taken from c0, a batch's first
// eight bones were never blended, and the pieces on them (the eyes, nose, cheeks) stood at the newer frame. Blended
// as Blend's matrices, each on its own. by_name: the older bones are these bones' own, found by their names
// (d3d::g_matrix_table), and when most of them jumped (a character put somewhere else) the draw is the newer
// frame's; paired by slot, a jump says nothing of this draw (the slot held someone else's) and all are blended.
constexpr uint32_t kBoneFirst = 64;  // c-32 in the constants as they're kept, from c-96
constexpr uint32_t kBoneGroups = 32;
constexpr uint32_t kBoneRegs = kBoneGroups * 4;
using Bones = std::array<std::array<float, 4>, kBoneRegs>;
Outcome BlendBones(const Bones& older, const Bones& newer, float t, Bones& out, bool by_name);

// whether these four registers are a perspective projection: (sx,0,x,0) (0,sy,x,0) (0,0,a,b) (0,0,+-1,0)
bool Perspective(const float r[4][4]);

// Frame N's draws matched to N-1's in tiers, as lmplied's match_frames: the same geometry (program, buffers, indices,
// textures, target), else the same shape (program, stride, primitive, count, textures, target: geometry the title
// writes again each frame, at another place in its buffer), else the same batch (program, stride, primitive,
// target: particles, whose count changes). Of the draws sharing a key not yet taken, the one whose matrices were
// placed nearest (several copies of one mesh: the title may draw them in another order each frame).
constexpr uint32_t kTiers = 3;
using Keys = std::array<uint64_t, kTiers>;

class Matcher {
public:
    // the frame's draws so far become the older frame's
    void NextFrame();
    // the draw's place in the older frame, or -1; records it in the newer, with its bones when it has them
    int32_t Match(const Keys& keys, const Regs& regs, const Bones* bones = nullptr, uint32_t groups = 0xF);
    const Regs& Older(int32_t i) const { return older_[size_t(i)].regs; }
    const Bones* OlderBones(int32_t i) const {
        int32_t b = older_[size_t(i)].bones;
        return b < 0 ? nullptr : &older_bones_[size_t(b)];
    }
    uint32_t LastTier() const { return last_tier_; }  // the tier the last match was made in, kTiers for none

private:
    struct Kept {
        Keys keys;
        Regs regs;
        int32_t bones = -1;  // in older_bones_ / newer_bones_
        uint32_t groups = 0xF;
    };
    struct Run {
        std::vector<uint32_t> draws;  // the older frame's draws with the key, in order
        uint32_t next = 0;            // the first not looked at yet
    };
    std::vector<Kept> older_, newer_;
    std::vector<Bones> older_bones_, newer_bones_;
    std::vector<uint8_t> taken_;  // the older frame's draws matched already
    std::array<std::unordered_map<uint64_t, Run>, kTiers> older_by_key_;
    uint32_t last_tier_ = kTiers;
    // The camera's step between frames, per group: of the draws matched exactly, the matrix most of them share
    // (the level), older = step x newer. The last frame's step foretells this one's: of the draws sharing a key,
    // the one where the step puts the draw is taken, not the nearest on screen, which a turn moves further than the
    // gap between two pieces of a wall or a map.
    struct Seen {
        uint32_t count = 0;
        std::array<std::array<float, 4>, 4> older, newer;
    };
    std::array<std::unordered_map<uint64_t, Seen>, kGroups> seen_;
    std::array<std::array<std::array<double, 4>, 4>, kGroups> step_{};
    std::array<bool, kGroups> have_step_{};
    double Apart(uint32_t older, const Regs& newer, uint32_t groups, const Bones* bones) const;
};

}  // namespace gpu::interp
