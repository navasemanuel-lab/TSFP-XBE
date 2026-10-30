// Name tags in Story co-op: each player sees the others' profile names above their heads, as in team games.
// The game draws them in team games only (0x13C560, bit 0 of the game's flags [0x7DE458]: a team game), over every
// character on the list [0x7A3580] (count [0x74C37C]), a player's name from 0x353C00(player). In Story co-op the
// drawing runs as in a team game over the players' characters alone, each named by their profile (0x69620: +0x7C).
// It keeps each view's tags in two buffers of 32 floats by the character's place on the list ([0x7451A0] and
// [0x746EBC], a view's each), made at the level's start in team games only (0x13C510): made in Story co-op too, and
// the players put first on the list while the tags are drawn.
#include <algorithm>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGameMode = 0x7DE454;  // byte 1: 10 in Story
constexpr uint32_t kFlags = 0x7DE458;     // bit 0: a team game
constexpr uint32_t kPlayers = 0x790950;
constexpr uint32_t kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584;
constexpr uint32_t kCharacters = 0x7A3580, kCharacterCount = 0x74C37C;
constexpr uint32_t kTagBuffers = 0x7451A0, kFadeBuffers = 0x746EBC, kTagSlots = 32;
constexpr uint32_t kViewPlayer = 0x7B0C7C;  // the player whose view is drawn: +4 the view
constexpr uint32_t kProfiles = 0x7BEDC4, kProfileSize = 0x1F54, kProfileName = 0x7C;
constexpr uint32_t kSessionUp = 0x76B108;  // System Link (Online Story): the game's own name, the network's

bool g_tags = false;  // in the tags' drawing for Story co-op

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
bool StoryCoop() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10 && kernel::R32(kPlayers) > 1; }

// a player's character: [player+0x14]+0x7C
uint32_t CharacterOf(uint32_t player) {
    uint32_t body = Valid(player, 0x18) ? kernel::R32(player + 0x14) : 0;
    return Valid(body, 0x80) ? kernel::R32(body + 0x7C) : 0;
}

// the profile's name when it is one: printable, ended within 32
bool ProfileName(uint32_t player, uint32_t& name) {
    name = kProfiles + player * kProfileSize + kProfileName;
    if (!Valid(name, 32)) return false;
    for (uint32_t i = 0; i < 32; i++) {
        uint8_t c = uint8_t(kernel::R32(name + i) & 0xFF);
        if (!c) return i > 0;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return false;
}

}  // namespace

// TagBuffers(): at the level's start, each view's buffers for the tags in a team game; in Story co-op too
X86_EXTERN_FUNC(__imp__sub_0013C510);
X86_FUNC(sub_0013C510) {
    uint32_t flags = kernel::R32(kFlags);
    if ((flags & 1) || !StoryCoop()) {
        __imp__sub_0013C510(ctx, base);
        return;
    }
    kernel::W32(kFlags, flags | 1);
    __imp__sub_0013C510(ctx, base);
    kernel::W32(kFlags, (kernel::R32(kFlags) & ~1u) | (flags & 1));
    for (uint32_t view = 0; view < 4; view++) {
        for (uint32_t buffers : {kTagBuffers, kFadeBuffers}) {
            uint32_t buffer = kernel::R32(buffers + view * 4);
            if (Valid(buffer, kTagSlots * 4))
                for (uint32_t i = 0; i < kTagSlots; i++) kernel::W32(buffer + i * 4, 0);
        }
    }
}

// TeamTags(): over the characters on the list, for the current player's view
X86_EXTERN_FUNC(__imp__sub_0013C560);
X86_FUNC(sub_0013C560) {
    uint32_t flags = kernel::R32(kFlags), count = kernel::R32(kCharacterCount);
    uint32_t players = std::min<uint32_t>(kernel::R32(kPlayers), 4), first = kernel::R32(kPlayerArray);
    uint32_t viewer = kernel::R32(kViewPlayer);
    uint32_t view = Valid(viewer, 8) ? kernel::R32(viewer + 4) : ~0u;
    if ((flags & 1) || !StoryCoop() || !game::Opt().player_names || count > 4096 || !Valid(kCharacters, count * 4) ||
        !Valid(first, players * kPlayerSize) || view >= 4 ||
        !Valid(kernel::R32(kTagBuffers + view * 4), kTagSlots * 4) ||
        !Valid(kernel::R32(kFadeBuffers + view * 4), kTagSlots * 4)) {
        __imp__sub_0013C560(ctx, base);
        return;
    }
    // the players' characters alone on the list for it, first: their places within the buffers
    std::vector<uint32_t> saved(count);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t character = saved[i] = kernel::R32(kCharacters + i * 4);
        bool player = false;
        for (uint32_t p = 0; p < players && character; p++) player |= CharacterOf(first + p * kPlayerSize) == character;
        if (player && found < kTagSlots) saved.push_back(character), found++;
    }
    static uint32_t logged = 0;
    if (kernel::R32(kSessionUp) && found != logged) {
        logged = found;
        LOGR("names: %u players' names over their heads online", found);
    }
    for (uint32_t i = 0; i < found; i++) kernel::W32(kCharacters + i * 4, saved[count + i]);
    kernel::W32(kCharacterCount, found);
    kernel::W32(kFlags, flags | 1);
    g_tags = true;
    if (found) __imp__sub_0013C560(ctx, base);
    else kernel::Return(ctx, 0, 0);
    g_tags = false;
    kernel::W32(kFlags, (kernel::R32(kFlags) & ~1u) | (flags & 1));
    kernel::W32(kCharacterCount, count);
    for (uint32_t i = 0; i < count; i++) kernel::W32(kCharacters + i * 4, saved[i]);
}

// PlayerName(player): for the tags in Story co-op, the player's profile name
X86_EXTERN_FUNC(__imp__sub_00353C00);
X86_FUNC(sub_00353C00) {
    uint32_t player = kernel::R32(ctx.esp + 4), name = 0;
    if (g_tags && player < 4 && !kernel::R32(kSessionUp) && ProfileName(player, name)) {
        kernel::Return(ctx, name, 0);
        return;
    }
    __imp__sub_00353C00(ctx, base);
}

// The tags' two markers (sprites 0xEB and 0xEE, drawn by 0x1BA70 from 0x13CF75 and 0x13D049) are team games' own: in
// Story co-op the names alone are drawn, the sprite hook (menu_mouse.cpp) skipping these
bool NameTagMarker(uint32_t ret) { return g_tags && (ret == 0x13CF7A || ret == 0x13D04E); }
