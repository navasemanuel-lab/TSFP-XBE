#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// the HDR10 output as the swap chain was made: 0 off, 1 on, -1 asked for with the display's HDR off in Windows
inline std::atomic<int> g_hdr_output{0};

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// The performance overlay's numbers (panel), measured while diag::Perf() and published every half second: per frame
// of the title, in milliseconds unless named otherwise
struct FrameStats {
    bool valid = false;
    double fps = 0, frame = 0, worst = 0;  // the title's frames, the longest of them
    double shown_fps = 0;                  // frames reaching the screen (more with frame interpolation)
    double game = 0;                       // the title's threads busy (their CPU time): the game, recording its draws
    double game_wait = 0;                  // the title's thread waiting on the render thread
    double pace = 0;                       // the title's thread asleep in the frame cap
    double render = 0, draw = 0, compile = 0, copy = 0;  // the render thread's work: in all, draws, shaders, copies
    double present = 0, swap = 0, vsync = 0;             // handing the frame over: in all, the swap, waiting on it
    double gpu = 0;
    double gpu_pass[8] = {};  // scene, full, small, resolve, copy, final, effects, other
    double audio = 0;         // the mixer, its share of one core in percent
    double draws = 0, tris = 0, upload_mb = 0, tex_uploads = 0;
    double shaders = 0, pipelines = 0;  // made this half second
    char threads[160] = {};             // the busiest threads, percent of a core
};
FrameStats Stats();

// visibility test results held at the highest of this many reports in a row: a title testing a sprite against the
// depth it lies on (a sun on its sky) counts pixels that flicker on the host's rasterisation; 1 reports each as is
inline std::atomic<uint32_t> g_report_hold{1};
// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

}  // namespace gpu
