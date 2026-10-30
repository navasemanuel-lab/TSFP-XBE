#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// while on, the buffers this thread starts play silent on this machine alone, until started again with it off: the
// title's own state is as if they played
void MutePlays(bool on);

}  // namespace audio
