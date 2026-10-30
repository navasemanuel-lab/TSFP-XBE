// Story for three or four players on one screen. The game takes Story with one or two: the main menu (0x2D0BE0)
// greys its Story row out past two local players ([0x5236C4], text 0x16AE in place of 0x16A2), and a Story level has
// two places to start from. The rest already counts the players: the mission's setup takes the local players
// (0x65940, [esi] = [0x5236C4]), the partner's character type is every player's past the first (0x64DD0), and the
// screens, cameras and HUD split for four as in Arcade.
//
// The menu is built here as for two (the Co-op Story row, 0x407). Players 3 and 4 are the partner's companions:
// - Spawn(player) (0xD4F50) puts a player at its start (player +0xA8 the place, +0x100 the facing, then 0xD2DE0 the
//   body, its room 0x9BCF0 and floor 0xC3C50); past the level's starts it is put at (index, 0, 0), out in the void.
//   Players 3 and 4 are put where player 2 is.
// - TeleportToPad(actor, pad, flags, facing) (0xD6F60, the scripts' through 0xDA280 and others) moves a character to a
//   pad; a player's (actor [0] the body, body +0x150 the player). When it moves player 2, it moves players 3 and 4 too.
#include <bit>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kLocalPlayers = 0x5236C4;
constexpr uint32_t kGameMode = 0x7DE454;  // its second byte 10 in Story
constexpr uint32_t kPlayers = 0x790950, kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584;
constexpr uint32_t kPlace = 0xA8, kFacing = 0x100, kBody = 0x14, kActor = 0x7C;
constexpr uint32_t kPlacePlayer = 0xD2DE0;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }
uint32_t Count() { return kernel::R32(kPlayers); }
uint32_t Player(uint32_t i) { return kernel::R32(kPlayerArray) + i * kPlayerSize; }
float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
uint32_t ActorOf(uint32_t player) {
    uint32_t body = Valid(player, kBody + 4) ? kernel::R32(player + kBody) : 0;
    return Valid(body, kActor + 4) ? kernel::R32(body + kActor) : 0;
}

struct Saved {
    X86Context& ctx;
    uint32_t r[7];
    explicit Saved(X86Context& c)
        : ctx(c), r{c.eax, c.ecx, c.edx, c.ebx, c.esi, c.edi, c.ebp} {}
    ~Saved() {
        ctx.eax = r[0], ctx.ecx = r[1], ctx.edx = r[2], ctx.ebx = r[3], ctx.esi = r[4], ctx.edi = r[5], ctx.ebp = r[6];
    }
};
}  // namespace

namespace game {
bool StoryOnlineOpening();                                    // game/story_online.cpp
bool StoryOnlineSpawnPad(X86Context& ctx, uint32_t context);  // game/story_online.cpp

// the main menu's builder (game/menu_layout.cpp) round this: its Story row offered to three and four players as to two
void StoryMenuForFour(bool building) {
    static uint32_t local = 0;
    if (building) {
        local = kernel::R32(kLocalPlayers);
        if (local <= 2 || local > 4) {
            local = 0;
            return;
        }
        kernel::W32(kLocalPlayers, 2);
        static int logged = 0;
        if (logged++ < 4) LOGR("story4: the main menu's Story row open to %u players", local);
    } else if (local) {
        kernel::W32(kLocalPlayers, local);
        local = 0;
    }
}
}  // namespace game

// StartPad(player): the Nth authored start of the level. A map made for two has none past the second and gives -1,
// and Spawn (0xD4F50) then reads a null pad. Players 3 and 4 take player 2's start in its place.
X86_EXTERN_FUNC(__imp__sub_000CF060);
X86_FUNC(sub_000CF060) {
    uint32_t player = kernel::R32(ctx.esp + 4);
    __imp__sub_000CF060(ctx, base);
    if (game::StoryOnlineSpawnPad(ctx, player)) return;  // online Story's opening: its own starts
    if (ctx.eax != 0xFFFFFFFFu || !Story() || Count() <= 2 || Count() > 4 || !Valid(player, kPlayerSize)) return;
    uint32_t index = kernel::R32(player);
    if (index < 2 || index >= Count() || player != Player(index)) return;
    kernel::W32(player, 1);
    uint32_t pad = kernel::CallGuest(ctx, 0xCF060, {player});
    kernel::W32(player, index);
    ctx.eax = pad;
    static int logged = 0;
    if (logged++ < 16) LOGR("story4: player %u has no start of its own, takes player 2's (pad %d)", index + 1, int32_t(pad));
}

// Spawn(player): in Story, players 3 and 4 start where player 2 did
X86_EXTERN_FUNC(__imp__sub_000D4F50);
X86_FUNC(sub_000D4F50) {
    uint32_t player = kernel::R32(ctx.esp + 4);
    __imp__sub_000D4F50(ctx, base);
    if (!Story() || game::StoryOnlineOpening() || Count() <= 2 || Count() > 4 || !Valid(player, kPlayerSize)) return;
    uint32_t index = kernel::R32(player);
    uint32_t partner = Player(1);
    if (index < 2 || index >= Count() || !ActorOf(partner) || !ActorOf(player)) return;
    Saved keep(ctx);
    for (uint32_t k = 0; k < 3; k++) kernel::W32(player + kPlace + k * 4, kernel::R32(partner + kPlace + k * 4));
    kernel::W32(player + kFacing, kernel::R32(partner + kFacing));
    kernel::CallGuest(ctx, kPlacePlayer, {player});
    LOGR("story4: player %u starts by player 2 (%.2f %.2f %.2f)", index + 1, F(player + kPlace),
         F(player + kPlace + 4), F(player + kPlace + 8));
}

// TeleportToPad(actor, pad, flags, facing): player 2 moved, players 3 and 4 with them
X86_EXTERN_FUNC(__imp__sub_000D6F60);
X86_FUNC(sub_000D6F60) {
    uint32_t actor = kernel::R32(ctx.esp + 4), pad = kernel::R32(ctx.esp + 8), flags = kernel::R32(ctx.esp + 12),
             facing = kernel::R32(ctx.esp + 16);
    __imp__sub_000D6F60(ctx, base);
    if (!Story() || Count() <= 2 || Count() > 4 || !actor || actor != ActorOf(Player(1))) return;
    Saved keep(ctx);
    for (uint32_t i = 2; i < Count(); i++) {
        uint32_t other = ActorOf(Player(i));
        if (!other) continue;
        kernel::CallGuest(ctx, 0xD6F60, {other, pad, flags, facing});
        static int logged = 0;
        if (logged++ < 64) LOGR("story4: player %u moved with player 2 to pad %u", i + 1, pad);
    }
}
