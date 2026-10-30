// Debug tools, active only with their environment variables set.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/debug.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace debug {

namespace {

// XBE_MEMDUMP_AT="<seconds>,...": the guest's committed memory (user space and the contiguous mirror) to
// memdump_<n>.bin at those times, as records of (address, bytes, data) per committed run of pages
void DumpMemory(int n) {
    char name[64];
    std::snprintf(name, sizeof name, "memdump_%d.bin", n);
    FILE* f = std::fopen(name, "wb");
    if (!f) return;
    auto dump = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t va = lo; va < hi;) {
            if (!mem::IsCommitted(va)) {
                va += mem::kPage;
                continue;
            }
            uint32_t end = va;
            while (end < hi && mem::IsCommitted(end)) end += mem::kPage;
            uint32_t size = end - va;
            std::fwrite(&va, 4, 1, f);
            std::fwrite(&size, 4, 1, f);
            std::fwrite(kernel::HostPtr(va), 1, size, f);
            va = end;
        }
    };
    dump(0x00010000, 0x08000000);
    dump(0x80000000, 0x84000000);
    std::fclose(f);
    LOGR("debug: memory dump %d written", n);
}

}  // namespace

void Tick() {
    static const std::vector<double> at = [] {
        std::vector<double> out;
        const char* s = std::getenv("XBE_MEMDUMP_AT");
        for (std::string rest = s ? s : ""; !rest.empty();) {
            size_t comma = rest.find(',');
            out.push_back(std::atof(rest.substr(0, comma).c_str()));
            rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        }
        return out;
    }();
    if (at.empty()) return;
    static const auto start = std::chrono::steady_clock::now();
    static size_t next = 0;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (next < at.size() && t >= at[next]) DumpMemory(int(next++));
}

}  // namespace debug
