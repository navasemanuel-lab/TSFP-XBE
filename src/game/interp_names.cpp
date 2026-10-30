// The names of the game's matrices, for frame interpolation (d3d::g_matrix_table, gpu/interp.h).
//
// Interpolation paired each draw with one of the last frame's by what it looked like, and blended its bones slot by
// slot. A character's slots are not its bones: a piece's palette is filled where its batch starts, so a slot held
// one bone in a frame and another in the next, and the piece was blended between two bones that were never the same
// one (the face pulled apart at its seams, the jaw left in another pose). The N64 recompilations (Zelda64Recomp, RT64)
// name every matrix in the game's code instead, an actor and a limb; here the game names them itself.
//
// A batch of a model (sub_00099340) fills the table at 0x66526C with pointers into the instance's matrices
// (instance +8, 64 bytes each, from the batch's first, +0x14) and its length at 0x665408; the vertex shader setups
// (sub_0002C8F0 and its siblings) then load entry k, transposed (sub_001530E0), into registers 0x40 + 4k. The pointer
// is the bone of that character, the same from frame to frame, whatever slot it lands in.

#include "xdk/d3d8.h"

namespace {

constexpr uint32_t kFirst = 0x40;        // the register entry 0 goes to
constexpr uint32_t kTable = 0x66526C;    // the batch's matrix pointers
constexpr uint32_t kCount = 0x665408;    // how many

[[maybe_unused]] const bool kNamed = [] {
    d3d::g_matrix_table = {kFirst, kTable, kCount};
    return true;
}();

}  // namespace
