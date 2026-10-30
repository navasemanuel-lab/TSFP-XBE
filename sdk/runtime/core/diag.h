#pragma once
#include <atomic>
#include <cstdlib>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set, or a measurement running (F9, ToggleMeasure): perf log lines once a second and the read-backs named
inline std::atomic<bool>& PerfFlag() {
    static std::atomic<bool> on{std::getenv("XBE_PERF") != nullptr};
    return on;
}
inline bool Perf() { return PerfFlag().load(std::memory_order_relaxed); }

// XBE_CAPTURE_DIR: where F3 saves a frame, "captures" beside the executable when unset, set empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path("captures");
    }();
    return dir;
}

// XBE_PROFILE=<from>-<to>: samples every thread's stack in that window of seconds, see profile.cpp
void StartProfiler();
// the title reads the file: starts a window of XBE_PROFILE=@<text>:<seconds> when the name holds the text
void ProfileFile(const char* name);
// Shift+F9: the perf lines and the threads' stacks sampled for that many seconds from now, into the log
void MeasureNow(double seconds);
// F9: the perf lines from now until F9 again, then a summary of the whole measurement; no stack sampling, which
// would cost time itself over a long one
void ToggleMeasure();
// a measured second: its frame rate and GPU milliseconds a frame, for the summary
void NoteSecond(double fps, double gpu_ms);

}  // namespace diag
