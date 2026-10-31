// Which behaviour each character in a level runs, logged as it changes. A character's AI data (its +0x7C) keeps its
// behaviour as a 64-bit mask at +0x10 and +0x14; each tick 0x27D590 (the character in esi) runs the routine of the
// bit set: 0x40000000 the ghosts' (0x2BA300: idle 0x249, trap 0x24A, attack 0x24B, death 0x24C, the anims of
// "ghostidle" and on), 0x8000000 and 0x1000000 ones that share 0x15CD00 with it, and so on. What Lies Below's
// ghosts (the dam's zombies and chrs/ghost of l_56_st.pak) are to be told apart here: a zombie that turns into one
// shows as a change to 0x40000000.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "core/log.h"
#include "xdk/hle.h"

namespace {

struct Seen {
    uint64_t type;
    uint32_t logged;
};
std::unordered_map<uint32_t, Seen> g_seen;
uint32_t g_lines = 0;
constexpr uint32_t kMostLines = 400;

}  // namespace

namespace game {

// Damage(record) (game/net_bots.cpp's sub_000BB6C0): +0 the character hit, +4 who hit it, +0x18 the damage type
// (0x19 the Ghost Gun's beam), +0x20 the amount, +0x40 flags. Logged for ghosts and for the beam, to see what reaches
// a ghost.
void LogDamage(uint32_t rec) {
    uint32_t chr = rec ? kernel::R32(rec) : 0;
    uint32_t ai = chr ? kernel::R32(chr + 0x7C) : 0, type = rec ? kernel::R32(rec + 0x18) : 0;
    bool ghost = ai && kernel::R32(ai + 0x10) == 0x40000000;
    static uint32_t lines = 0;
    if ((ghost || type == 0x19) && lines < 200) {
        lines++;
        uint32_t spawner = ai ? kernel::R32(ai + 0xBF0) : 0;
        float amount;
        uint32_t bits = kernel::R32(rec + 0x20);
        std::memcpy(&amount, &bits, 4);
        LOGR("ai: damage to %08X (behaviour %X, state %u, chr+28 %X, spawner flags %X) type %X amount %.3f flags %X",
             chr, ai ? kernel::R32(ai + 0x10) : 0, ai ? kernel::R32(ai + 0x20) : 0, kernel::R32(chr + 0x28),
             spawner ? kernel::R32(spawner + 0x58) : 0, type, amount, kernel::R32(rec + 0x40));
    }
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_0027D590);
X86_FUNC(sub_0027D590) {
    uint32_t chr = ctx.esi, ai = chr ? kernel::R32(chr + 0x7C) : 0;
    if (ai && g_lines < kMostLines) {
        uint64_t type = uint64_t(kernel::R32(ai + 0x14)) << 32 | kernel::R32(ai + 0x10);
        auto [it, fresh] = g_seen.try_emplace(chr, Seen{type, 0});
        if (fresh || it->second.type != type) {
            g_lines++;
            char was[40] = "";
            if (!fresh) std::snprintf(was, sizeof(was), " (was %llX)", (unsigned long long)it->second.type);
            LOGR("ai: character %08X (ai %08X) behaviour %llX%s | ai +0 %08X %08X %08X %08X +18 %08X %08X", chr, ai,
                 (unsigned long long)type, was, kernel::R32(ai), kernel::R32(ai + 4), kernel::R32(ai + 8),
                 kernel::R32(ai + 0xC), kernel::R32(ai + 0x18), kernel::R32(ai + 0x1C));
            it->second.type = type;
        }
    }
    __imp__sub_0027D590(ctx, base);
}
