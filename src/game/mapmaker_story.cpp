// MapMaker story maps: any character, alone or in co-op. A MapMaker map is played as an arcade game (mode 9) whose story
// game type is 2. The character list (0x2C6C80) gives each player Cortez (character 0xC) for that type; the game type
// list (0x2C9F00) and its rows' text (0x2C9C10) grey story out past one player. With a MapMaker map the character list
// is built as for the other types, every unlocked character, and the story type is offered to two players.
#include <algorithm>

#include "core/log.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kMode = 0x78AD2C;       // 9 arcade (a MapMaker map is played as one)
constexpr uint32_t kType = 0x78ADAE;       // byte: the game type, 2 story
constexpr uint32_t kArcadeMap = 0x78ADAC;  // byte: the map chosen, 0x64 a MapMaker map
constexpr uint32_t kMap = 0x79094C;
constexpr uint32_t kPlayers = 0x5236C4;
constexpr uint32_t kArcade = 9, kStory = 2, kMapMakerMap = 0x64;
constexpr uint32_t kOtherArcadeMode = 0xB;  // a mode whose list holds every character
constexpr uint32_t kCoopPlayers = 2;

uint8_t Byte(uint32_t va) { return uint8_t(kernel::R32(va) & 0xFF); }
bool MapMakerArcade() { return kernel::R32(kMode) == kArcade && Byte(kArcadeMap) == kMapMakerMap; }

void Trace(const char* where) {
    static uint32_t last[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
    uint32_t now[5] = {kernel::R32(kMode), Byte(kType), Byte(kArcadeMap), kernel::R32(kMap), kernel::R32(kPlayers)};
    if (std::equal(now, now + 5, last)) return;
    std::copy(now, now + 5, last);
    LOGR("mapmaker: %s mode %u type %u arcade map %u map %d players %u", where, now[0], now[1], now[2], int32_t(now[3]),
         now[4]);
}

// runs fn with the player count read as one, for the story type's two-player test
template <typename Fn>
void AsOnePlayer(Fn fn) {
    uint32_t players = kernel::R32(kPlayers);
    bool coop = MapMakerArcade() && players == kCoopPlayers;
    if (coop) kernel::W32(kPlayers, 1);
    fn();
    if (coop) kernel::W32(kPlayers, players);
}

}  // namespace

// CharacterList(): who can be chosen

X86_EXTERN_FUNC(__imp__sub_002C6C80);
X86_FUNC(sub_002C6C80) {
    Trace("characters");
    bool any = MapMakerArcade() && Byte(kType) == kStory;
    if (any) kernel::W32(kMode, kOtherArcadeMode);
    __imp__sub_002C6C80(ctx, base);
    if (any) kernel::W32(kMode, kArcade);
}

// GameTypeList(menu, header, select): the game types a map can be played as
X86_EXTERN_FUNC(__imp__sub_002C9F00);
X86_FUNC(sub_002C9F00) {
    Trace("game types");
    AsOnePlayer([&] { __imp__sub_002C9F00(ctx, base); });
}

// a game type row's text: its description, or why it cannot be played
X86_EXTERN_FUNC(__imp__sub_002C9C10);
X86_FUNC(sub_002C9C10) {
    AsOnePlayer([&] { __imp__sub_002C9C10(ctx, base); });
}

// GameSetup(): the players' characters for the level (0x78AD6C, costume 0x78AD9C), Cortez for each in arcade's story
// type (at 0x2D35A6). With a MapMaker map each local player keeps the one chosen, as the other types take it: the
// player's profile (0x69620) +0x60 the character, +0x64 its costume where the character has them (0x2BE270).
X86_EXTERN_FUNC(__imp__sub_002D2C70);
X86_FUNC(sub_002D2C70) {
    __imp__sub_002D2C70(ctx, base);
    if (!MapMakerArcade() || Byte(kType) != kStory) return;
    constexpr uint32_t kPresent = 0x3568C0, kOnline = 0x3533F0, kProfile = 0x69620, kHasCostumes = 0x2BE270;
    constexpr uint32_t kCharacters = 0x78AD6C, kCostumes = 0x78AD9C;
    uint32_t players = std::min<uint32_t>(kernel::R32(kPlayers), 4);
    if (kernel::CallGuest(ctx, kOnline, {})) return;
    for (uint32_t i = 0; i < players; i++) {
        if (!kernel::CallGuest(ctx, kPresent, {i})) continue;
        uint32_t profile = kernel::CallGuest(ctx, kProfile, {i});
        if (!profile) continue;
        uint8_t character = uint8_t(kernel::R32(profile + 0x60));
        *static_cast<uint8_t*>(kernel::HostPtr(kCharacters + i)) = character;
        if (kernel::CallGuest(ctx, kHasCostumes, {character}))
            *static_cast<uint8_t*>(kernel::HostPtr(kCostumes + i)) = uint8_t(kernel::R32(profile + 0x64));
        LOGR("mapmaker: story player %u plays character %u", i + 1, character);
    }
}
