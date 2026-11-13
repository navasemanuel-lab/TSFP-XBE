#pragma once
#include <atomic>
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// a copy swap's D3DSWAP_FINISH alone waits for the next frame as the console's vertical sync does; off, a title that
// shows each frame with two swaps runs at the full frame rate
inline std::atomic<bool> g_pace_finish{true};
// draws, clears and copies are counted
inline std::atomic<uint32_t> g_draw_count{0};
// A title's names for its matrices, which frame interpolation pairs by (gpu/interp.h) instead of guessing which of
// the last frame's draws was which, as RT64's matrix groups do for the N64 recompilations: the registers from
// `first` take, four at a time, the transposes of the 4x4s at the guest pointers at `table` (as many as the count at
// `count`). A group set to its pointer's matrix is named by the pointer (State::vsc_id); table 0, none.
struct MatrixTable {
    uint32_t first = 0, table = 0, count = 0;
};
inline MatrixTable g_matrix_table;
// the vertical blank count as the blank thread last set it, after the blank's callback ran, and whether that thread
// runs
inline std::atomic<uint32_t> g_blank_count{0};
inline std::atomic<bool> g_blanking{false};
// the blank count now, to take before looking at what the title's blank callback counts
inline uint32_t BlankSeen() { return g_blank_count.load(); }
// a title's look at an indexed draw before it is made: its index count and indices, where its positions (v1) are and
// their stride; a title mending a model's data in guest memory does it here
inline void (*g_indexed_draw_hook)(uint32_t count, uint32_t indices, uint32_t positions, uint32_t stride) = nullptr;
// a title's look at a draw of vertices in order before it is made: its primitive type, first vertex and count, where its
// positions (v1) are and their stride; true leaves the draw out
inline bool (*g_vertices_draw_hook)(uint32_t prim, uint32_t start, uint32_t count, uint32_t positions,
                                    uint32_t stride) = nullptr;
// sleeps until the blank count is no longer `seen` (BlankSeen), for a title that polls it in a loop: taken before
// the title's own count was found unmoved, a blank in between is not slept through
inline void WaitBlank(uint32_t seen) {
    if (g_blanking.load(std::memory_order_relaxed)) g_blank_count.wait(seen);
}

}  // namespace d3d
