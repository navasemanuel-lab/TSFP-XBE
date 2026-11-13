#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// nanoseconds spent mixing, for the performance overlay (added by the mixer, taken by the reader)
inline std::atomic<uint64_t> g_mix_ns{0};

// the speakers the output plays on, set by StartOutput before it returns: 2 (front left, right) or 6 (5.1: front
// left, right, centre, sub, back left, right), interleaved in that order; mono: one picture on both of 2
inline std::atomic<uint32_t> g_channels{2};
inline std::atomic<bool> g_mono{false};

// the platform output pulls interleaved float frames of g_channels from render; returns once g_channels is known
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

}  // namespace audio
