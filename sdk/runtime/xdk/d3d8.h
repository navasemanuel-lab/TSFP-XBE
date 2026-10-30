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
// draws, clears and copies are counted, and while g_hide_draws is set not made: the game draws a view that is not
// this machine's to show (online co-op, each machine shows its own player's view)
inline std::atomic<bool> g_hide_draws{false};
inline std::atomic<uint32_t> g_draw_count{0};
// A title's names for its matrices, which frame interpolation pairs by (gpu/interp.h) instead of guessing which of
// the last frame's draws was which, as RT64's matrix groups do for the N64 recompilations: the registers from
// `first` take, four at a time, the transposes of the 4x4s at the guest pointers at `table` (as many as the count at
// `count`). A group set to its pointer's matrix is named by the pointer (State::vsc_id); table 0, none.
struct MatrixTable {
    uint32_t first = 0, table = 0, count = 0;
};
inline MatrixTable g_matrix_table;
// each Swap that shows a frame, on the thread that made it, before it is shown
inline void (*g_on_swap)() = nullptr;
// the vertical blank count as the blank thread last set it, after the blank's callback ran, and whether that thread
// runs
inline std::atomic<uint32_t> g_blank_count{0};
inline std::atomic<bool> g_blanking{false};
// the blank count now, to take before looking at what the title's blank callback counts
inline uint32_t BlankSeen() { return g_blank_count.load(); }
// sleeps until the blank count is no longer `seen` (BlankSeen), for a title that polls it in a loop: taken before
// the title's own count was found unmoved, a blank in between is not slept through
inline void WaitBlank(uint32_t seen) {
    if (g_blanking.load(std::memory_order_relaxed)) g_blank_count.wait(seen);
}

}  // namespace d3d
