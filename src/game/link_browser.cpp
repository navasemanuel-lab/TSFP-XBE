// System Link's game list keeps the games it has found while it searches again.
//
// The list's transport (vtable 0x541DC0) keeps up to 8 games of 0x54 at 0x665538 in a pool at 0x665894 (+0 the
// entries, +4 the first free, +8 the free count, +0xC the count; a free entry's +0 the next free; one in use has +0x3C
// set and +0x40 its age in polls, dropped past 10 refreshes of 60, asked again past 3). The browser (0x1CBAC0) starts
// a new search (0x2DCF0, through 0x352980) each time results come, and the search empties the pool before it asks
// again. On a LAN the answers come back the same frame; over a VPN the list is empty until they do, and the games
// blink out once a second. The games found are put back after the search starts: their ages go on, so a host that
// stops answering still leaves the list.
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kEntries = 0x665538, kEntrySize = 0x54, kCount = 8, kInUse = 0x3C;
constexpr uint32_t kPool = 0x665894;  // +4 the first free, +8 the free count
}  // namespace

// StartSearch(): the games found kept
X86_EXTERN_FUNC(__imp__sub_0002DCF0);
X86_FUNC(sub_0002DCF0) {
    uint8_t kept[kEntrySize * kCount];
    std::memcpy(kept, kernel::HostPtr(kEntries), sizeof kept);
    __imp__sub_0002DCF0(ctx, base);
    uint32_t found = 0;
    for (uint32_t i = 0; i < kCount; i++) {
        uint32_t flag;
        std::memcpy(&flag, kept + i * kEntrySize + kInUse, 4);
        found += flag != 0;
    }
    if (!found) return;
    std::memcpy(kernel::HostPtr(kEntries), kept, sizeof kept);
    uint32_t head = 0, free = 0;
    for (uint32_t i = 0; i < kCount; i++) {
        uint32_t entry = kEntries + i * kEntrySize;
        if (kernel::R32(entry + kInUse)) continue;
        kernel::W32(entry, head);
        head = entry, free++;
    }
    kernel::W32(kPool + 4, head);
    kernel::W32(kPool + 8, free);
    static int logged = 0;
    if (logged++ < 4) LOGR("link: the search starts again, its %u games kept", found);
}
