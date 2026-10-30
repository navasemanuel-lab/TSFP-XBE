// XBE_PROFILE=<from>-<to> (seconds after start): every thread's host call stack sampled once a millisecond in that
// window, then the functions most often on each busy thread's stack and at its top written to the log.
// XBE_PROFILE=@<text>:<seconds>: a window that long each time the title reads a file whose name holds the text
// Shift+F9 (MeasureNow): a window from that moment, with the perf lines (diag::Perf) on while it lasts
// F9 (ToggleMeasure): the perf lines alone until F9 again, then the measurement summed up
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/diag.h"
#include "core/log.h"

namespace diag {

namespace {

constexpr int kDepth = 32;
constexpr size_t kMaxSamples = 400000;

struct Sample {
    DWORD tid;
    uint8_t n;
    DWORD64 pc[kDepth];
};

// the return address at rsp, 0 when it cannot be read
DWORD64 ReadReturn(DWORD64 rsp) {
    __try {
        return *reinterpret_cast<const DWORD64*>(rsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// the suspended thread's stack from its context, without allocating (the thread may hold the heap lock)
uint8_t Walk(CONTEXT c, DWORD64* pc) {
    uint8_t n = 0;
    while (n < kDepth && c.Rip) {
        pc[n++] = c.Rip;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fe) {
            c.Rip = ReadReturn(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &handler_data, &frame, nullptr);
    }
    return n;
}

std::string Name(DWORD64 pc, std::unordered_map<DWORD64, std::string>& cache) {
    auto it = cache.find(pc);
    if (it != cache.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::string name;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym)) {
        name = sym->Name;
        // far from the nearest name (a function with no public symbol, or a DLL's export): the distance, to 0x100
        if (disp >= 0x1000) {
            char tail[32];
            std::snprintf(tail, sizeof(tail), "+%llX?", (unsigned long long)(disp & ~0xFFull));
            name += tail;
        }
    } else {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%llX", (unsigned long long)pc);
        name = hex;
    }
    return cache[pc] = name;
}

void Report(const std::vector<Sample>& samples, double ms) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::unordered_map<DWORD64, std::string> names;
    std::map<DWORD, std::vector<const Sample*>> by_thread;
    for (const Sample& s : samples) by_thread[s.tid].push_back(&s);
    LOGR("profile: %zu samples over %.0f ms", samples.size(), ms);
    for (auto& [tid, list] : by_thread) {
        std::unordered_map<std::string, size_t> incl, leaf;
        size_t busy = 0;
        for (const Sample* s : list) {
            std::unordered_set<std::string> seen;
            for (int i = 0; i < s->n; i++) {
                std::string nm = Name(s->pc[i], names);
                if (i == 0) leaf[nm]++;
                if (seen.insert(nm).second) incl[nm]++;
            }
        }
        // threads that only wait are left out
        for (auto& [nm, c] : leaf)
            if (nm.find("ZwWaitFor") == std::string::npos && nm.find("NtWaitFor") == std::string::npos &&
                nm.find("ZwDelayExecution") == std::string::npos && nm.find("NtDelayExecution") == std::string::npos &&
                nm.find("ZwRemoveIoCompletion") == std::string::npos)
                busy += c;
        if (list.size() < 50) continue;
        LOGR("profile: thread %lu: %zu samples, %zu not waiting", tid, list.size(), busy);
        auto top = [&](std::unordered_map<std::string, size_t>& m, const char* what, size_t count) {
            std::vector<std::pair<size_t, std::string>> v;
            for (auto& [nm, c] : m) v.push_back({c, nm});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < std::min(count, v.size()); i++)
                LOGR("profile:   %s %5.1f%% %s", what, 100.0 * double(v[i].first) / double(list.size()),
                     v[i].second.c_str());
        };
        top(incl, "on stack", 40);
        top(leaf, "top     ", 15);
    }
    SymCleanup(GetCurrentProcess());
}

std::atomic<bool> g_running{false};
std::string g_trigger;
double g_trigger_secs = 0;

void Run(double from, double to) {
    auto start = std::chrono::steady_clock::now();
    auto at = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    while (at() < from) Sleep(5);
    std::vector<Sample> samples;
    samples.reserve(kMaxSamples);
    std::vector<std::pair<DWORD, HANDLE>> threads;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    double listed = -1.0, begun = at();
    while (at() < to && samples.size() + threads.size() < kMaxSamples) {
        if (at() - listed > 0.1) {
            listed = at();
            for (auto& [id, h] : threads) CloseHandle(h);
            threads.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      te.th32ThreadID);
                if (h) threads.push_back({te.th32ThreadID, h});
            }
            CloseHandle(snap);
        }
        for (auto& [id, h] : threads) {
            if (SuspendThread(h) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            Sample s{id, 0, {}};
            if (GetThreadContext(h, &c)) s.n = Walk(c, s.pc);
            ResumeThread(h);
            if (s.n) samples.push_back(s);
        }
        Sleep(1);
    }
    for (auto& [id, h] : threads) CloseHandle(h);
    Report(samples, (at() - begun) * 1000.0);
    logx::Flush();
    g_running = false;
}

}  // namespace

void StartProfiler() {
    const char* s = std::getenv("XBE_PROFILE");
    if (!s) return;
    if (s[0] == '@') {
        std::string v = s + 1;
        size_t colon = v.rfind(':');
        if (colon == std::string::npos) return;
        g_trigger = v.substr(0, colon);
        g_trigger_secs = std::atof(v.c_str() + colon + 1);
        return;
    }
    double from = 0, to = 0;
    if (std::sscanf(s, "%lf-%lf", &from, &to) != 2 || to <= from) return;
    g_running = true;
    std::thread([from, to] { Run(from, to); }).detach();
}

void ProfileFile(const char* name) {
    if (g_trigger.empty() || !std::strstr(name, g_trigger.c_str()) || g_running.exchange(true)) return;
    LOGR("profile: %s read, sampling %.2f s", name, g_trigger_secs);
    double secs = g_trigger_secs;
    std::thread([secs] { Run(0, secs); }).detach();
}

namespace {

struct Measure {
    std::mutex lock;
    bool on = false, was = false;
    std::chrono::steady_clock::time_point start;
    double seconds = 0, fps = 0, lowest = 0, gpu = 0, highest = 0;
    uint32_t under = 0;  // seconds under 58 fps
};
Measure g_measure;

}  // namespace

void ToggleMeasure() {
    std::lock_guard lock(g_measure.lock);
    Measure& m = g_measure;
    if (!m.on) {
        if (g_running) return;  // a timed window runs
        m.on = true;
        m.was = PerfFlag().exchange(true);
        m.start = std::chrono::steady_clock::now();
        m.seconds = m.fps = m.gpu = m.highest = 0;
        m.lowest = 1e9;
        m.under = 0;
        LOGR("profile: measuring until F9");
        return;
    }
    m.on = false;
    PerfFlag() = m.was;
    double length = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.start).count();
    if (m.seconds > 0)
        LOGR("profile: measured %.0f s: %.1f fps average, lowest second %.1f, %u s under 58 fps (%.1f%%) | gpu %.2f ms "
             "a frame average, highest second %.2f",
             length, m.fps / m.seconds, m.lowest, m.under, 100.0 * m.under / m.seconds, m.gpu / m.seconds, m.highest);
    else
        LOGR("profile: measured %.0f s, too short for a second of perf lines", length);
    logx::Flush();
}

void NoteSecond(double fps, double gpu_ms) {
    std::lock_guard lock(g_measure.lock);
    Measure& m = g_measure;
    if (!m.on) return;
    m.seconds++;
    m.fps += fps;
    m.gpu += gpu_ms;
    m.lowest = std::min(m.lowest, fps);
    m.highest = std::max(m.highest, gpu_ms);
    if (fps < 58) m.under++;
}

void MeasureNow(double seconds) {
    if (g_measure.on || g_running.exchange(true)) return;
    bool was = PerfFlag().exchange(true);
    LOGR("profile: measuring %.0f s from now (F9)", seconds);
    std::thread([seconds, was] {
        Run(0, seconds);
        PerfFlag() = was;
        LOGR("profile: measurement done");
        logx::Flush();
    }).detach();
}

}  // namespace diag

#else
namespace diag {
void StartProfiler() {}
void ProfileFile(const char*) {}
void MeasureNow(double) {}
void ToggleMeasure() {}
void NoteSecond(double, double) {}
}  // namespace diag
#endif
