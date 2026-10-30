// Bots in System Link games, after the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc,
// GPLv3, docs/online-bots-investigation.md and src/pc_network_bots*): the host picks a bot set in its lobby's options,
// its game runs them, and the others show what the host's bots do.
//
// The lobby: the host's options show Arcade's own Bot Set row (the menus built while the menu mode [0x78AD2C] reads
// Arcade's 9, then put back to the network's 11 to 13). The editor keeps 10 bots of 7 words at 0x75FD90. The match's
// settings [0x78AD58] get the bots after the humans (characters +0x14, teams +0x24, skills +0x34, variants +0x44, the
// humans' count +0, the bots' +2) just before the game sends them (0x356490) and before the host loads the level
// (0x658D0): the game's own builder clears the bot count after it builds them (0x2D38C0). Humans come first, bots fill
// what the map's limit for the mode leaves (0x4D239C: 17 maps of 15 words, column 2 for mode 11, 3 for 13, 0 for 12).
// The lobby's population line (from 0x197707, in its draw 0x197890) says players + bots / limit.
//
// The match ([0x7DE400] is its settings, the bots' actors in the pool [0x7356D8] by slot, 0xEA0 each, the body at
// +0): the game checks network players by their controller records (0x3568C0), which bots don't have; the callers
// that go through every actor (updates, radar, HUD, scores) take the bots' slots too. Network references to an actor
// (0x359400 writes, 0x3594B0 reads, 0x3593D0 asks) get a kind 4 with the bot's slot. The host owns the bots: its
// game runs them, decides their deaths (0x35CC10) and every 3 ticks sends each bot's state to the others as a side
// datagram (xdk/xnet.h): place, angles, health, flags, animation channels, aim, hit bounds, scores and the last shot
// it fired (0x1BB5C0). The others put each state on their own body of that bot (made again when the host's comes
// back to life, 0xDAC30), pose it without moving its animation on (0x5FDC0), fire the shot for the show and say
// "you killed" when the host's scores say so (0xCF440). Their hits on a bot go to the host (0x35C870) and take no
// health here (0xBB6C0); the host's state brings the result. The others put their bots on the game's list of
// characters and take them off it by the host's states, and nothing is left on it without a body (explosions go
// through it and read each body).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "xdk/xnet.h"

namespace game {  // game/story_online.cpp
bool StoryOnline();
bool StoryOnlineLobbyHeader(X86Context& ctx);
void StoryOnlineLobbyConfig();
uint32_t StoryOnlinePlayerLimit();
bool StoryOnlinePrepare(X86Context& ctx, uint32_t address);
bool StoryOnlineActorActive(uint32_t caller, uint32_t index, uint32_t entity);
bool StoryOnlineInLevel();
}  // namespace game

namespace {

using kernel::R32;
using kernel::W32;

constexpr uint32_t kSessionUp = 0x76B108, kRole = 0x76B140, kLobbyState = 0x76B1A4;  // role 1 host, 2 joined
constexpr uint32_t kRemoteEvent = 0x76B170;  // a network event from another game being applied
constexpr uint32_t kLobbyReady = 11, kInGame = 15;
constexpr uint32_t kMenuMode = 0x78AD2C, kMenuMap = 0x78AD4C, kMenuConfig = 0x78AD58, kArcadeMode = 9;
constexpr uint32_t kPlayConfig = 0x7DE400, kTick = 0x7DE318;
constexpr uint32_t kRoster = 0x75FD90, kRosterSize = 10;
constexpr uint32_t kNetPlayers = 0x77728C, kNetPlayerSize = 0x124;  // +0: bit 0 a player there
constexpr uint32_t kCapacities = 0x4D239C, kMaps = 17;
constexpr uint32_t kActors = 0x7356D8, kActorSize = 0xEA0;
constexpr uint32_t kAiBodies = 0x7A2D00, kNpcCount = 0x7A2958, kLevelPlayers = 0x790950;
constexpr uint32_t kPlayers = 0x7B0C48, kPlayerSize = 0x1584;
constexpr uint32_t kClipIndex = 0x7E03C0;
constexpr uint32_t kCharacters = 0x7A3580, kCharacterCount = 0x74C37C, kCharacterRoom = 32;
constexpr uint32_t kConfigBytes = 0xA0, kSlots = 16, kWeapons = 66;
constexpr uint32_t kActorSlots = 32;  // the actors a match's states name: a Story level's NPCs go up to 31
constexpr uint32_t kSendTicks = 3;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
uint8_t* At(uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); }
uint32_t U(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
bool Finite(const float* f, int n, float limit) {
    for (int i = 0; i < n; i++)
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > limit) return false;
    return true;
}

// ---- the lobby ----

struct RosterBot {
    int32_t character, team, skill, enabled, override_enabled, preview, variant;
};
struct Population {
    uint32_t players = 0, human_slots = 0, selected = 0, bots = 0, waiting = 0, limit = 0;
};

bool g_enabled = false;  // the host has opened its bot options this run
bool g_lobby_drawing = false;

bool Selected(const RosterBot& b) {
    int on = b.override_enabled == -1 ? b.enabled : b.override_enabled;
    return on == 1 && b.character >= 0 && b.character <= 255 && b.skill >= 0 && b.skill <= 4 && b.team >= 0 &&
           b.team <= 3 && b.variant >= -1 && b.variant <= 255;
}

bool LobbyNetwork() {
    uint32_t mode = R32(kMenuMode);
    return R32(kSessionUp) && mode >= 11 && mode <= 13;
}

// the map's player limit in this mode, as 0x2BEA00 reads it; 0 for a map not on the table
uint32_t Limit() {
    uint32_t mode = R32(kMenuMode), map = R32(kMenuMap);
    uint32_t column = mode == 11 ? 2 : mode == 13 ? 3 : 0;
    if (map >= kMaps) return 0;
    return std::min(R32(kCapacities + (map * 15 + column) * 4), kSlots);
}

// the players and the bots that fit; configure: the host's match settings get them
Population Prepare(bool configure) {
    Population p;
    p.limit = Limit();
    for (uint32_t i = 0; i < kSlots; i++)
        if (R32(kNetPlayers + i * kNetPlayerSize) & 1) p.players++, p.human_slots = i + 1;
    RosterBot roster[kRosterSize];
    std::memcpy(roster, At(kRoster), sizeof roster);
    if (g_enabled)
        for (const RosterBot& b : roster) p.selected += Selected(b);
    // the players' ids can't move while they are joined: bots go after the highest
    uint32_t room = p.human_slots < p.limit ? p.limit - p.human_slots : 0;
    p.bots = std::min(p.selected, room);
    p.waiting = p.selected - p.bots;
    if (!configure || !LobbyNetwork() || R32(kRole) != 1 || !g_enabled) return p;
    uint8_t* config = At(kMenuConfig);
    uint32_t humans = p.human_slots;
    if (!humans || !config[1] || config[1] > 4 || config[1] > humans) return p;
    uint32_t teams = config[0x56] == 4 ? 4 : 2, count = 0;
    for (const RosterBot& b : roster) {
        if (count >= p.bots) break;
        if (!Selected(b)) continue;
        uint32_t slot = humans + count++;
        config[0x14 + slot] = uint8_t(b.character);
        config[0x24 + slot] = (config[0x58] & 1) ? uint8_t(1 + uint32_t(b.team) % teams) : 0;
        config[0x34 + slot] = uint8_t(b.skill);
        config[0x44 + slot] = uint8_t(b.variant);
    }
    config[0] = uint8_t(humans);
    if (config[2] != count) LOGR("netbots: %u players, %u bots", humans, count);
    config[2] = uint8_t(count);
    return p;
}

// ---- the match ----

// Online Story (game/story_online.cpp): the level's NPCs, every actor after the players, are the host's as bots are
bool Story() { return game::StoryOnlineInLevel(); }
uint32_t Humans() { return At(kPlayConfig)[0]; }
uint32_t Bots() { return Story() ? kActorSlots - std::min(Humans(), kActorSlots) : At(kPlayConfig)[2]; }
bool MatchBots() {
    uint32_t h = Humans(), b = Bots();
    if (Story()) return h && h <= 4;
    return h && b && b <= kRosterSize && h + b <= kSlots;
}
bool InGame(uint32_t role) { return R32(kSessionUp) && R32(kRole) == role && R32(kLobbyState) == kInGame; }

// a bot's actor by its slot, 0 for a player's or outside a network match with bots
uint32_t BotActor(uint32_t slot) {
    if (!R32(kSessionUp) || !MatchBots() || slot < Humans() || slot >= Humans() + Bots()) return 0;
    uint32_t pool = R32(kActors);
    return pool ? pool + slot * kActorSize : 0;
}
uint32_t BotBody(uint32_t slot) {
    uint32_t actor = BotActor(slot);
    return actor ? R32(actor) : 0;
}
uint32_t BotSlot(uint32_t body) {
    if (body)
        for (uint32_t slot = 0; slot < kActorSlots; slot++)
            if (BotBody(slot) == body) return slot;
    return kActorSlots;
}

// what the host sends: numbers only, never an address in its memory
struct Shot {
    uint32_t serial, tick, spawn, hand, mode, flags, target_slot;
    int32_t weapon;
    float transform[12], aim[3], muzzle[3];
};
struct State {
    uint32_t slot, tick, entity_flags, flags[3], life[4], weapon;
    uint32_t body_present;
    float position[3], health, armor;
    uint32_t last_damage_tick, spawn;
    float bounds_center[3], bounds_radius[3];
    float max_health, health_scale[2];
    float angles[3], heading, aim[3], muzzle[3];
    int32_t clip;
    float clip_channels[18];
    uint32_t clip_cycle, clip_flags;
    int32_t next_clip;
    float next_channels[18];
    uint32_t next_cycle, next_flags, overlay_phase;
    float overlay_channels[8], blend_channels[3];
    Shot shot;
    int16_t scores[42];
    int16_t killed_by[kActorSlots];  // this bot's column of the kill table: who killed it how often
    int16_t kind[2];                 // a Story NPC's characters (actor +0x48, +0x4A) and its variant (+0xC0D)
    uint32_t variant;
};
struct Packet {
    uint32_t tag, match;
    State state;
};
constexpr uint32_t kTag = 0x31544F42;  // "BOT1"

bool ShotValid(const Shot& s, uint32_t actors) {
    if (!s.serial) return true;
    if (s.hand > 1 || s.mode > 1 || s.flags > 1 || s.weapon < 0 || s.weapon >= int32_t(kWeapons) ||
        (s.target_slot >= actors && s.target_slot != UINT32_MAX))
        return false;
    return Finite(s.transform, 12, 1e6f) && Finite(s.aim, 3, 1e6f) && Finite(s.muzzle, 3, 1e6f);
}
bool StateValid(const State& s, uint32_t humans, uint32_t bots) {
    if (!humans || !bots || humans + bots > kActorSlots || s.slot < humans || s.slot >= humans + bots) return false;
    if (s.weapon >= 70 || s.body_present > 1 || s.kind[0] < -1 || s.kind[1] < -1 || s.variant > 255) return false;
    if (!Finite(s.position, 3, 1e6f) || !Finite(s.bounds_center, 3, 1e4f) || !Finite(s.bounds_radius, 3, 1e4f))
        return false;
    for (float r : s.bounds_radius)
        if (r < 0) return false;
    if (!Finite(s.health_scale, 2, 1000) || !Finite(s.angles, 3, 1e5f) || !Finite(&s.heading, 1, 1e5f)) return false;
    if (s.clip < 0 || s.clip > 4095 || s.next_clip < 0 || s.next_clip > 4095 || s.overlay_phase > 2) return false;
    if (!Finite(s.aim, 3, 1e6f) || !Finite(s.muzzle, 3, 1e6f) || !Finite(s.clip_channels, 18, 1e6f) ||
        !Finite(s.next_channels, 18, 1e6f) || !Finite(s.overlay_channels, 8, 1e6f) || !Finite(s.blend_channels, 3, 1e6f))
        return false;
    if (!ShotValid(s.shot, humans + bots)) return false;
    return std::isfinite(s.health) && s.health >= -1000 && s.health <= 1000 && std::isfinite(s.armor) &&
           s.armor >= 0 && s.armor <= 1000 && std::isfinite(s.max_health) && s.max_health >= 0 &&
           s.max_health <= 1000;
}

// a match's identity: the host's states for another match are dropped
uint32_t MatchId() {
    const uint8_t* c = At(kPlayConfig);
    return c[0x99] | uint32_t(c[0x54]) << 8 | uint32_t(c[0x55]) << 16 | uint32_t(c[0x56]) << 24;
}

// the states received, taken by the game's thread (the network's thread puts them here)
struct Wire {
    std::mutex lock;
    uint32_t role = 0, humans = 0, bots = 0, match = 0;
    State pending[kActorSlots]{};
    bool fresh[kActorSlots]{}, seen[kActorSlots]{};
    uint32_t last_tick[kActorSlots]{};
    bool heard = false;
} g_wire;

void Context(uint32_t role, uint32_t match, uint32_t humans, uint32_t bots) {
    std::lock_guard<std::mutex> lock(g_wire.lock);
    if (g_wire.role == role && g_wire.match == match && g_wire.humans == humans && g_wire.bots == bots) return;
    g_wire.role = role, g_wire.match = match, g_wire.humans = humans, g_wire.bots = bots;
    std::fill(std::begin(g_wire.fresh), std::end(g_wire.fresh), false);
    std::fill(std::begin(g_wire.seen), std::end(g_wire.seen), false);
    g_wire.heard = false;
}

void Receive(const uint8_t* data, size_t len, uint32_t) {
    Packet p;
    if (len != sizeof p) return;
    std::memcpy(&p, data, sizeof p);
    if (p.tag != kTag) return;
    std::lock_guard<std::mutex> lock(g_wire.lock);
    if (g_wire.role != 2 || p.match != g_wire.match || !StateValid(p.state, g_wire.humans, g_wire.bots)) return;
    uint32_t i = p.state.slot;
    if (g_wire.seen[i] && int32_t(p.state.tick - g_wire.last_tick[i]) <= 0) return;
    if (!g_wire.heard) {
        g_wire.heard = true;
        LOGR("netbots: the host's bots are coming in");
    }
    g_wire.pending[i] = p.state;
    g_wire.fresh[i] = g_wire.seen[i] = true;
    g_wire.last_tick[i] = p.state.tick;
}
const bool g_receiving = (xnet::SideReceiver(Receive), true);

bool Take(uint32_t slot, State& s) {
    std::lock_guard<std::mutex> lock(g_wire.lock);
    if (slot >= kActorSlots || g_wire.role != 2 || !g_wire.fresh[slot]) return false;
    s = g_wire.pending[slot];
    g_wire.fresh[slot] = false;
    return true;
}

void Send(const State& s) {
    Packet p{kTag, MatchId(), s};
    xnet::SideSend(&p, sizeof p);
}

Shot g_shots[kActorSlots]{};  // the host: each bot's last shot
struct Bounds {
    uint32_t body, spawn;
    float center[3], radius[3];
} g_bounds[kActorSlots]{};  // the others: each bot's hit bounds as the host has them

// the others: a Story NPC's body as the host made it (the kind, the variant, the place), where this game's own level
// made another or none: this game's Story spawns none of its own (0x15DF20 below), the host's states make them
uint32_t g_making = 0;
void List(uint32_t actor, bool on);
uint32_t MakeNpc(X86Context& ctx, uint32_t slot, const State& s) {
    uint32_t pool = R32(kActors), players = R32(kLevelPlayers);
    if (!pool || s.kind[0] < 0 || s.kind[1] < 0 || slot < players || slot - players >= 30 || !InGame(2)) return 0;
    uint32_t actor = pool + slot * kActorSize;
    if (!Valid(actor, kActorSize)) return 0;
    if (uint32_t old = R32(actor)) {
        if (!Valid(old, 0x25C) || R32(old + 0x7C) != actor) return 0;
        List(actor, false);
        kernel::CallGuest(ctx, 0xA9070, {old});
        W32(actor, 0);
        W32(kAiBodies + (slot - players) * 4, 0);
    }
    g_making++;
    uint32_t body = kernel::CallGuest(ctx, 0xDAC30, {uint32_t(int32_t(s.kind[0])), uint32_t(int32_t(s.kind[1])), slot,
                                                     U(s.position[0]), U(s.position[1]), U(s.position[2]),
                                                     U(s.heading), UINT32_MAX, 1, s.variant});
    g_making--;
    if (!body || !Valid(body, 0x25C) || R32(actor) != body) return 0;
    W32(kAiBodies + (slot - players) * 4, body);
    kernel::CallGuest(ctx, 0xD7570, {actor});
    if (R32(kNpcCount) < slot - players + 1) W32(kNpcCount, slot - players + 1);
    return body;
}
bool SameKind(uint32_t actor, const State& s) {
    int16_t kind[2];
    std::memcpy(kind, At(actor + 0x48), 4);
    return kind[0] == s.kind[0] && kind[1] == s.kind[1];
}

// the others: a bot's body made again, as Arcade makes it, after the host's came back to life
uint32_t Revive(X86Context& ctx, uint32_t slot) {
    uint32_t pool = R32(kActors);
    if (Story() || !MatchBots() || slot < Humans() || slot >= Humans() + Bots() || !pool || !InGame(2)) return 0;
    uint32_t actor = pool + slot * kActorSize;
    if (!Valid(actor, kActorSize) || R32(actor)) return 0;
    uint32_t character = At(kPlayConfig + 0x14)[slot];
    uint32_t variant = uint32_t(int32_t(int8_t(At(kPlayConfig + 0x44)[slot])));
    uint32_t body = kernel::CallGuest(ctx, 0xDAC30, {character, character, slot, 0, 0, 0, 0, UINT32_MAX, 1, variant});
    if (!body || !Valid(body, 0x25C) || R32(actor) != body) return 0;
    W32(kAiBodies + (slot - Humans()) * 4, body);
    kernel::CallGuest(ctx, 0xD7570, {actor});
    return body;
}

// The game's list of characters [0x7A3580] (actors, count [0x74C37C]): explosions, mines and the like go through it
// (0xC15B0) and read each one's body, so one there without a body crashes them. The game adds a character when it
// comes to life (0x15BCA0) and takes it off when it dies (0x15BCC0); the others' bots live and die by the host's
// states instead, so they are put on and off it here, and any character left there without a body is taken off.
uint32_t ListedAt(uint32_t actor) {
    uint32_t count = std::min(R32(kCharacterCount), kCharacterRoom);
    for (uint32_t i = 0; i < count; i++)
        if (R32(kCharacters + i * 4) == actor) return i;
    return UINT32_MAX;
}
void Unlist(uint32_t i) {
    uint32_t count = std::min(R32(kCharacterCount), kCharacterRoom);
    if (i >= count) return;
    W32(kCharacters + i * 4, R32(kCharacters + (count - 1) * 4));
    W32(kCharacters + (count - 1) * 4, 0);
    W32(kCharacterCount, count - 1);
}
void List(uint32_t actor, bool on) {
    uint32_t i = ListedAt(actor), count = R32(kCharacterCount);
    if (!on) {
        if (i != UINT32_MAX) Unlist(i);
    } else if (i == UINT32_MAX && count < kCharacterRoom) {
        W32(kCharacters + count * 4, actor);
        W32(kCharacterCount, count + 1);
    }
}
void TidyCharacters() {
    if (R32(kCharacterCount) > kCharacterRoom) W32(kCharacterCount, kCharacterRoom);
    for (uint32_t i = 0; i < R32(kCharacterCount);) {
        uint32_t actor = R32(kCharacters + i * 4);
        uint32_t body = Valid(actor, kActorSize) ? R32(actor) : 0;
        bool twice = false;
        for (uint32_t k = 0; k < i && !twice; k++) twice = R32(kCharacters + k * 4) == actor;
        if (!Valid(body, 0x2E8) || R32(body + 0x7C) != actor || twice) {
            LOG_FIRST(10, "netbots: character %08X off the list (body %08X%s)", actor, body, twice ? ", twice" : "");
            Unlist(i);
        } else {
            i++;
        }
    }
}

// a body moved by hand: its rotation made again (+0x28 bit 0x1000) and its place among the map's sectors
void Refresh(X86Context& ctx, uint32_t body) {
    if (!Valid(body, 0x2E8)) return;
    W32(body + 0x28, R32(body + 0x28) | 0x1000);
    kernel::CallGuest(ctx, 0x9FBE0, {body});
}

// the host's animation on the others' body: the clips by their index, the channels as numbers
void Present(X86Context& ctx, uint32_t body, const State& s) {
    if (!Valid(body, 0x238)) return;
    uint32_t actor = R32(body + 0x7C);
    if (Valid(actor, kActorSize))
        for (uint32_t hand = 0; hand < 2; hand++) {
            uint32_t weapon = R32(actor + 0x18C + hand * 0x130);
            if (!Valid(weapon, 0x2C)) continue;
            bool shown = s.body_present && s.health > 0 && (s.entity_flags & 1);
            W32(weapon + 0x28, shown ? R32(weapon + 0x28) | 1 : R32(weapon + 0x28) & ~1u);
        }
    if (!s.body_present) return;
    uint32_t model = R32(body + 4);
    if (!Valid(model, 0x200)) return;
    if (int32_t(R32(model + 0xD4)) != s.clip) kernel::CallGuest(ctx, 0x966E0, {model, uint32_t(s.clip), U(s.clip_channels[11])});
    std::memcpy(At(model + 0xDC), s.clip_channels, 72);
    // 0xC00: root motion still to apply, which the host's place already has
    W32(model + 0x124, s.clip_cycle);
    W32(model + 0x128, s.clip_flags & ~0xC00u);
    int16_t next_index;
    std::memcpy(&next_index, At(kClipIndex + uint32_t(s.next_clip) * 2), 2);
    W32(model + 0x12C, uint32_t(s.next_clip));
    W32(model + 0x130, uint32_t(int32_t(next_index)));
    std::memcpy(At(model + 0x134), s.next_channels, 72);
    W32(model + 0x17C, s.next_cycle);
    W32(model + 0x180, s.next_flags & ~0xC00u);
    W32(model + 0xA8, s.overlay_phase);
    std::memcpy(At(model + 0xAC), s.overlay_channels, 32);
    std::memcpy(At(model + 0x198), s.blend_channels, 12);
}

// the host's shot fired again on the others' body, for the show: their own target and weapon model
void PresentShot(X86Context& ctx, uint32_t body, const Shot& shot) {
    if (!Valid(body, 0x25C) || shot.hand > 1 || shot.mode > 1 || shot.flags > 1 || shot.weapon < 0 ||
        shot.weapon >= int32_t(kWeapons) || shot.target_slot >= kActorSlots || !InGame(2))
        return;
    uint32_t actor = R32(body + 0x7C), pool = R32(kActors);
    if (!Valid(actor, kActorSize) || !Valid(pool, kActorSlots * kActorSize)) return;
    uint32_t target = R32(pool + shot.target_slot * kActorSize);
    if (!Valid(target, 0x25C)) return;
    uint32_t weapon = actor + 0xA4 + shot.hand * 0x130;
    if (R32(weapon + 0x30) != uint32_t(shot.weapon) || !Valid(R32(weapon + 0xE8), 0x24)) return;
    uint32_t previous = R32(actor + 4);
    W32(actor + 4, target);
    std::memcpy(At(weapon), shot.transform, 48);
    std::memcpy(At(actor + 0xB88), shot.aim, 12);
    std::memcpy(At(actor + 0xBB4), shot.muzzle, 12);
    kernel::CallGuest(ctx, 0x1BB5C0, {body, shot.hand, shot.mode, shot.flags});
    W32(actor + 4, previous);
}

// "you killed" for a player of this game when the host's table says they killed a bot; false: try again later
bool KillNotice(X86Context& ctx, uint32_t victim, uint32_t killer) {
    uint32_t pool = R32(kActors), players = R32(kPlayers), humans = Humans();
    if (killer >= humans || victim >= kActorSlots || killer == victim) return true;
    if (!Valid(pool, kActorSlots * kActorSize) || !Valid(players, humans * kPlayerSize)) return false;
    uint32_t player = players + killer * kPlayerSize;
    if (int32_t(R32(player + 4)) < 0) return true;  // another game's player
    uint32_t source = R32(player + 0x14);
    if (!R32(player + 8) || !Valid(source, 0x25C) || R32(source + 0x7C) != pool + killer * kActorSize) return false;
    kernel::CallGuest(ctx, 0xCF440, {player, pool + victim * kActorSize});
    return true;
}

void Frame(X86Context& ctx) {
    static uint32_t last_tick = 0;
    static bool sent = false;
    static uint32_t seen_shots[kActorSlots]{};
    static int16_t seen_kills[kActorSlots][kActorSlots]{};
    uint32_t role = R32(kRole);
    if (R32(kLobbyState) != kInGame || !R32(kSessionUp) || !MatchBots() || (role != 1 && role != 2)) {
        Context(0, 0, 0, 0);
        sent = false;
        std::memset(g_shots, 0, sizeof g_shots);
        std::memset(seen_shots, 0, sizeof seen_shots);
        std::memset(g_bounds, 0, sizeof g_bounds);
        std::memset(seen_kills, 0, sizeof seen_kills);
        return;
    }
    uint32_t pool = R32(kActors);
    if (!Valid(pool, kActorSlots * kActorSize)) return;
    uint32_t humans = Humans(), bots = Bots(), tick = R32(kTick);
    bool story = Story();
    static bool announced = false;
    if (story && !announced) LOGR("netbots: Story's NPCs %s", role == 1 ? "sent to the others" : "taken from the host");
    announced = story;
    Context(role, MatchId(), humans, bots);
    TidyCharacters();
    // a Story NPC every other tick: the others' own AI moves it between the host's states
    uint32_t interval = story ? 2 : kSendTicks;
    if (role == 1 && sent && tick - last_tick < interval) return;
    for (uint32_t slot = humans; slot < humans + bots; slot++) {
        uint32_t body = BotBody(slot), actor = pool + slot * kActorSize;
        State s;
        if (role == 2) {
            if (!Take(slot, s)) continue;
            // the scores outlive the body: this bot's row, and its column of who killed it (Story keeps none)
            if (!story) std::memcpy(At(actor + 0xA10), s.scores, sizeof s.scores);
            for (uint32_t killer = 0; killer < humans + bots && !story; killer++) {
                int16_t& seen = seen_kills[slot][killer];
                if (s.killed_by[killer] < seen) seen = s.killed_by[killer];
                while (s.killed_by[killer] > seen && KillNotice(ctx, slot, killer)) seen++;
                std::memcpy(At(pool + killer * kActorSize + 0xA10 + slot * 2), &s.killed_by[killer], 2);
            }
            if (!s.body_present) {
                if (body) {
                    W32(body + 0x28, R32(body + 0x28) & ~0x71u);
                    W32(body + 0x1CC, 0);
                    Present(ctx, body, s);
                }
                List(actor, false);
                g_bounds[slot] = {};
                continue;
            }
            if (story && body && Valid(body, 0x2E8) && !SameKind(actor, s)) {
                int16_t kind[2];
                std::memcpy(kind, At(actor + 0x48), 4);
                LOG_FIRST(20, "netbots: NPC %u is character %d here, %d at the host's: made as the host's", slot,
                          kind[0], s.kind[0]);
                body = MakeNpc(ctx, slot, s);
            }
            if (!body && s.health > 0 && (s.entity_flags & 1)) {
                body = story ? MakeNpc(ctx, slot, s) : Revive(ctx, slot);
                if (body) LOG_FIRST(40, "netbots: %s %u made (character %d)", story ? "NPC" : "bot", slot, s.kind[0]);
            }
            if (!body || !Valid(body, 0x2E8) || R32(body + 0x7C) != actor) {
                List(actor, false);
                continue;
            }
            // on the list while the host's is alive (actor +0x2C bit 4: dead), as the game's own come and go
            List(actor, s.health > 0 && (s.entity_flags & 1) && !(s.flags[2] & 4));
            // flags 0x71: drawn, hit, takes damage
            W32(body + 0x28, (R32(body + 0x28) & ~0x71u) | (s.entity_flags & 0x71));
            // actor +0x2C bit 0x10 is this game's own camera's: seen
            uint32_t seen_bit = R32(actor + 0x2C) & 0x10;
            std::memcpy(At(actor + 0x24), s.flags, 12);
            W32(actor + 0x2C, (s.flags[2] & ~0x10u) | seen_bit);
            std::memcpy(At(actor + 0x10), s.life, 16);
            W32(actor + 0x94, s.weapon);
            std::memcpy(At(body + 0x64), s.position, 12);
            std::memcpy(At(actor + 0x30), s.position, 12);
            std::memcpy(At(actor + 0xBB4), s.muzzle, 12);
            std::memcpy(At(actor + 0xB88), s.aim, 12);
            W32(body + 0x1CC, U(s.health));
            W32(body + 0x1D0, U(s.armor));
            W32(body + 0x1D4, s.last_damage_tick);
            W32(body + 0x1D8, U(s.max_health));
            std::memcpy(At(actor + 0xB08), s.health_scale, 8);
            std::memcpy(At(body + 0xC0), s.angles, 12);
            W32(body + 0x70, U(s.heading));
            W32(body + 0x258, s.spawn);
            Bounds& b = g_bounds[slot];
            b.body = body, b.spawn = s.spawn;
            std::memcpy(b.center, s.bounds_center, 12);
            std::memcpy(b.radius, s.bounds_radius, 12);
            std::memcpy(At(body + 0x1A8), s.bounds_center, 12);
            std::memcpy(At(body + 0x1C0), s.bounds_radius, 12);
            Present(ctx, body, s);
            Refresh(ctx, body);
            if (s.shot.serial != seen_shots[slot]) {
                seen_shots[slot] = s.shot.serial;
                if (s.shot.serial && s.shot.spawn == s.spawn && s.tick - s.shot.tick <= 30 &&
                    s.shot.target_slot < humans + bots && s.health > 0)
                    PresentShot(ctx, body, s.shot);
            }
            // posing and the sectors may have made their own bounds: the host's again
            std::memcpy(At(body + 0x1A8), s.bounds_center, 12);
            std::memcpy(At(body + 0x1C0), s.bounds_radius, 12);
            continue;
        }
        std::memset(&s, 0, sizeof s);
        s.slot = slot;
        s.tick = tick;
        if (!story) {
            std::memcpy(s.scores, At(actor + 0xA10), sizeof s.scores);
            for (uint32_t killer = 0; killer < humans + bots; killer++)
                std::memcpy(&s.killed_by[killer], At(pool + killer * kActorSize + 0xA10 + slot * 2), 2);
        }
        // no body: said so, or a lost death would leave the others' copy standing (a Story level's many empty
        // slots once a second)
        if (!body || !Valid(body, 0x2E8)) {
            if (!story || tick % 60 < interval) Send(s);
            continue;
        }
        s.body_present = 1;
        uint32_t flags = R32(body + 0x28), life = R32(actor + 0x18), dead = R32(actor + 0x2C);
        float health;
        std::memcpy(&health, At(body + 0x1CC), 4);
        // a network match leaves the bots' bodies off: a living one is drawn and hit (0x61)
        // (a Story NPC is drawn as the level's scripts have it)
        if (!story && life && life != 2 && !(dead & 4) && health > 0 && (flags & 0x61) != 0x61) {
            flags |= 0x61;
            W32(body + 0x28, flags);
            Refresh(ctx, body);
        }
        s.entity_flags = flags & 0x71;
        std::memcpy(s.flags, At(actor + 0x24), 12);
        std::memcpy(s.life, At(actor + 0x10), 16);
        s.weapon = R32(actor + 0x94);
        std::memcpy(s.position, At(body + 0x64), 12);
        s.health = health;
        std::memcpy(&s.armor, At(body + 0x1D0), 4);
        s.last_damage_tick = R32(body + 0x1D4);
        std::memcpy(&s.max_health, At(body + 0x1D8), 4);
        std::memcpy(s.health_scale, At(actor + 0xB08), 8);
        std::memcpy(s.angles, At(body + 0xC0), 12);
        std::memcpy(&s.heading, At(body + 0x70), 4);
        std::memcpy(s.aim, At(actor + 0xB88), 12);
        std::memcpy(s.muzzle, At(actor + 0xBB4), 12);
        s.spawn = R32(body + 0x258);
        std::memcpy(s.kind, At(actor + 0x48), 4);
        s.variant = At(actor + 0xC0D)[0];
        std::memcpy(s.bounds_center, At(body + 0x1A8), 12);
        std::memcpy(s.bounds_radius, At(body + 0x1C0), 12);
        uint32_t model = R32(body + 4);
        if (Valid(model, 0x200)) {
            s.clip = int32_t(R32(model + 0xD4));
            std::memcpy(s.clip_channels, At(model + 0xDC), 72);
            s.clip_cycle = R32(model + 0x124);
            s.clip_flags = R32(model + 0x128);
            s.next_clip = int32_t(R32(model + 0x12C));
            std::memcpy(s.next_channels, At(model + 0x134), 72);
            s.next_cycle = R32(model + 0x17C);
            s.next_flags = R32(model + 0x180);
            s.overlay_phase = R32(model + 0xA8);
            std::memcpy(s.overlay_channels, At(model + 0xAC), 32);
            std::memcpy(s.blend_channels, At(model + 0x198), 12);
        }
        s.shot = g_shots[slot];
        if (StateValid(s, humans, bots)) Send(s);
        else LOG_FIRST(5, "netbots: bot %u's state out of range, not sent", slot);
    }
    if (role == 1) last_tick = tick, sent = true;
}

// the menus that show the bot options: built as in Arcade for the host in its lobby
void AsArcade(X86Context& ctx, uint8_t* base, void (*original)(X86Context&, uint8_t*)) {
    uint32_t mode = R32(kMenuMode);
    bool host = R32(kSessionUp) && R32(kRole) == 1 && mode >= 11 && mode <= 13;
    if (host) {
        W32(kMenuMode, kArcadeMode);
        if (!g_enabled) LOGR("netbots: bot options open for this lobby");
        g_enabled = true;
    }
    original(ctx, base);
    if (host) W32(kMenuMode, mode);
}

}  // namespace

namespace game {
// the host's match settings, before they are sent or loaded (0x356490, 0x658D0 in coop_online.cpp)
void NetBotsConfig(uint32_t config) {
    if (config == kMenuConfig && !game::StoryOnlinePlayerLimit()) Prepare(true);
}
}  // namespace game

#define AS_ARCADE(fn)                                    \
    X86_EXTERN_FUNC(__imp__##fn);                        \
    X86_FUNC(fn) { AsArcade(ctx, base, __imp__##fn); }
AS_ARCADE(sub_002C78F0)  // the lobby's options
AS_ARCADE(sub_002D5E70)
AS_ARCADE(sub_002C8370)  // the options' footer
AS_ARCADE(sub_002C2C20)  // Setup Bots
AS_ARCADE(sub_002C0490)
AS_ARCADE(sub_002C19F0)
AS_ARCADE(sub_002C1570)
AS_ARCADE(sub_002C1490)
#undef AS_ARCADE

// SendSettings(config): the match settings to the others
X86_EXTERN_FUNC(__imp__sub_00356490);
X86_FUNC(sub_00356490) {
    game::NetBotsConfig(R32(ctx.esp + 4));
    game::StoryOnlinePrepare(ctx, R32(ctx.esp + 4));
    __imp__sub_00356490(ctx, base);
}

// LobbySend(): the lobby's settings to the others, again while it waits
X86_EXTERN_FUNC(__imp__sub_00354240);
X86_FUNC(sub_00354240) {
    if (LobbyNetwork() && R32(kLobbyState) == kLobbyReady && !game::StoryOnlinePlayerLimit()) Prepare(true);
    game::StoryOnlineLobbyConfig();
    __imp__sub_00354240(ctx, base);
}

// HumanCapacity(): at most the map's limit
X86_EXTERN_FUNC(__imp__sub_0035E630);
X86_FUNC(sub_0035E630) {
    __imp__sub_0035E630(ctx, base);
    if (uint32_t story = game::StoryOnlinePlayerLimit()) {
        ctx.eax = std::min(ctx.eax, story);
        return;
    }
    if (LobbyNetwork() && g_enabled)
        if (uint32_t limit = Limit()) ctx.eax = std::min(ctx.eax, limit);
}

// StartMatch(): not with more players than the map takes once bots are on
X86_EXTERN_FUNC(__imp__sub_002D38C0);
X86_FUNC(sub_002D38C0) {
    if (LobbyNetwork() && R32(kRole) == 1 && g_enabled) {
        Population p = Prepare(false);
        if (p.selected && (!p.limit || p.human_slots > p.limit)) {
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_002D38C0(ctx, base);
}

// LobbyDraw(): its population line says the bots too
X86_EXTERN_FUNC(__imp__sub_00197890);
X86_FUNC(sub_00197890) {
    if (game::StoryOnlineLobbyHeader(ctx)) return;  // a Story lobby's header: the mission
    bool previous = g_lobby_drawing;
    g_lobby_drawing = LobbyNetwork();
    __imp__sub_00197890(ctx, base);
    g_lobby_drawing = previous;
}

// Format(out, ...): the lobby's population line, from 0x197707 (its buffer has room for 112)
X86_EXTERN_FUNC(__imp__sub_0003D7F0);
X86_FUNC(sub_0003D7F0) {
    uint32_t caller = R32(ctx.esp), out = R32(ctx.esp + 4);
    __imp__sub_0003D7F0(ctx, base);
    if (!g_lobby_drawing || caller != 0x197707 || !Valid(out, 64)) return;
    Population p = Prepare(false);
    if (R32(kRole) != 1) {
        uint32_t bots = At(kMenuConfig)[2];
        p.bots = bots <= kRosterSize ? bots : 0, p.waiting = 0;
    }
    if (!p.bots && !p.waiting) return;
    char* text = reinterpret_cast<char*>(At(out));
    if (!p.limit || p.human_slots > p.limit)
        std::snprintf(text, 64, "Too many players - limit %u", p.limit);
    else if (p.waiting)
        std::snprintf(text, 64, "%u players + %u bots / %u (%u waiting)", p.players, p.bots, p.limit, p.waiting);
    else
        std::snprintf(text, 64, "%u players + %u bots / %u", p.players, p.bots, p.limit);
}

// Active(slot): a network player there; for the callers that go through every actor, a bot too
X86_EXTERN_FUNC(__imp__sub_003568C0);
X86_FUNC(sub_003568C0) {
    if (game::StoryOnlineActorActive(R32(ctx.esp), R32(ctx.esp + 4), ctx.ebx)) {
        kernel::Return(ctx, 1, 0);
        return;
    }
    switch (R32(ctx.esp)) {
        case 0x064743:  // team score order
        case 0x064856:  // scores
        case 0x064A06:  // a round's scores
        case 0x06562D:  // the winner
        case 0x13F82A:  // scoreboard rows
        case 0x2E72B6:  // paused Scores: team rows
        case 0x2E744B:  // paused Scores: rows
        case 0x0D8E5D:  // actor update
        case 0x133BC9:  // radar
        case 0x13E0BB:  // HUD character list
        case 0x13E230:
        case 0x13EAFE:
        case 0x13EBDA:
            if (BotActor(R32(ctx.esp + 4))) {
                kernel::Return(ctx, 1, 0);
                return;
            }
            break;
    }
    __imp__sub_003568C0(ctx, base);
}

// Serializable(body): a bot can be named over the network
X86_EXTERN_FUNC(__imp__sub_003593D0);
X86_FUNC(sub_003593D0) {
    if (BotSlot(R32(ctx.esp + 4)) < kActorSlots) {
        kernel::Return(ctx, 1, 0);
        return;
    }
    __imp__sub_003593D0(ctx, base);
}

// WriteActor(ecx body, eax cursor): a bot as kind 4 and its slot
X86_EXTERN_FUNC(__imp__sub_00359400);
X86_FUNC(sub_00359400) {
    uint32_t slot = BotSlot(ctx.ecx);
    if (slot < kActorSlots) {
        uint32_t at = R32(ctx.eax);
        At(at)[0] = 4;
        At(at)[1] = uint8_t(slot);
        W32(ctx.eax, at + 2);
        ctx.esp += 4;
        return;
    }
    __imp__sub_00359400(ctx, base);
}

// ReadActor(ecx cursor): kind 4 is a bot by its slot
X86_EXTERN_FUNC(__imp__sub_003594B0);
X86_FUNC(sub_003594B0) {
    uint32_t at = R32(ctx.ecx);
    if (At(at)[0] == 4 && R32(kSessionUp)) {
        uint32_t body = BotBody(At(at)[1]);
        W32(ctx.ecx, at + 2);
        kernel::Return(ctx, body, 0);
        return;
    }
    __imp__sub_003594B0(ctx, base);
}

// NetworkFrame(): after the game's own, the bots' states sent or put on
X86_EXTERN_FUNC(__imp__sub_003559A0);
X86_FUNC(sub_003559A0) {
    __imp__sub_003559A0(ctx, base);
    Frame(ctx);
}

// Damage(context): on the others a bot takes no health (the host's state brings it): their hits on one go to the
// host, and a bot's own shots, played here for the show, hurt no one
X86_EXTERN_FUNC(__imp__sub_000BB6C0);
X86_FUNC(sub_000BB6C0) {
    if (R32(kSessionUp) && R32(kRole) == 2 && MatchBots()) {
        uint32_t context = R32(ctx.esp + 4);
        bool remote = R32(kRemoteEvent) != 0;
        uint32_t source = Valid(context, 0x58) ? R32(context + 4) : 0;
        if (!remote && Valid(source, 0x80)) {
            uint32_t owner = R32(source + 0x7C), type = R32(source + 0x20), pool = R32(kActors);
            for (uint32_t slot = Humans(); slot < Humans() + Bots(); slot++)
                if (BotBody(slot) == source || (type == 8 && pool && owner == pool + slot * kActorSize)) {
                    kernel::Return(ctx, 0, 0);
                    return;
                }
        }
        if (Valid(context, 0x58) && BotSlot(R32(context)) < kActorSlots) {
            if (!remote) kernel::CallGuest(ctx, 0x35C870, {context});
            kernel::Return(ctx, 0, 0);
            return;
        }
    }
    __imp__sub_000BB6C0(ctx, base);
}

// DeathAllowed(context): a bot's death is the host's to decide
X86_EXTERN_FUNC(__imp__sub_0035CC10);
X86_FUNC(sub_0035CC10) {
    uint32_t context = R32(ctx.esp + 4);
    if (Valid(context, 4) && BotSlot(R32(context)) < kActorSlots) {
        kernel::Return(ctx, R32(kRole) == 1, 0);
        return;
    }
    __imp__sub_0035CC10(ctx, base);
}

// ScoreNetwork(): a bot's score rows take Arcade's way
X86_EXTERN_FUNC(__imp__sub_003533F0);
X86_FUNC(sub_003533F0) {
    uint32_t caller = R32(ctx.esp), slot = UINT32_MAX;
    if (caller == 0x06486E) slot = ctx.ebx;
    else if (caller == 0x064B18 || caller == 0x064BA5) slot = ctx.edi;
    else if (caller == 0x063F05) slot = ctx.esi;  // the finished match's table; later ESI is an actor there
    if (slot < kActorSlots && BotActor(slot)) {
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_003533F0(ctx, base);
}

// NpcFire(body, hand, mode, flags): the host keeps each bot's last shot for its state
X86_EXTERN_FUNC(__imp__sub_001BB5C0);
X86_FUNC(sub_001BB5C0) {
    uint32_t body = R32(ctx.esp + 4), hand = R32(ctx.esp + 8), mode = R32(ctx.esp + 12), flags = R32(ctx.esp + 16);
    uint32_t slot = InGame(1) ? BotSlot(body) : kActorSlots;
    Shot shot{};
    if (slot < kActorSlots && hand <= 1 && mode <= 1 && flags <= 1) {
        uint32_t actor = R32(body + 0x7C);
        if (Valid(actor, kActorSize)) {
            uint32_t target = R32(actor + 4), pool = R32(kActors);
            shot.hand = hand, shot.mode = mode, shot.flags = flags;
            shot.weapon = int32_t(R32(actor + 0xD4 + hand * 0x130));
            std::memcpy(shot.transform, At(actor + 0xA4 + hand * 0x130), 48);
            std::memcpy(shot.aim, At(actor + 0xB88), 12);
            std::memcpy(shot.muzzle, At(actor + 0xBB4), 12);
            shot.target_slot = UINT32_MAX;
            for (uint32_t i = 0; i < kActorSlots && target; i++)
                if (pool && R32(pool + i * kActorSize) == target) shot.target_slot = i;
            shot.tick = R32(kTick);
            shot.spawn = R32(body + 0x258);
        } else {
            slot = kActorSlots;
        }
    } else {
        slot = kActorSlots;
    }
    __imp__sub_001BB5C0(ctx, base);
    if (slot < kActorSlots && ctx.eax && shot.weapon >= 0 && shot.weapon < int32_t(kWeapons)) {
        shot.serial = g_shots[slot].serial + 1;
        if (!shot.serial) shot.serial = 1;
        if (ShotValid(shot, Humans() + Bots())) g_shots[slot] = shot;
    }
}

// SpawnNpc(record, ...): a Story level's NPC coming in at a free slot; on the others none (-1, as when there's no
// room, which its caller 0x164207 takes): the host's spawns come in its states, in the host's slots
X86_EXTERN_FUNC(__imp__sub_0015DF20);
X86_FUNC(sub_0015DF20) {
    if (Story() && InGame(2) && !g_making) {
        LOG_FIRST(5, "netbots: this game's own NPC spawn left to the host");
        kernel::Return(ctx, UINT32_MAX, 0);
        return;
    }
    __imp__sub_0015DF20(ctx, base);
}

// InReach(eax character's body, edx thing), from the things that go through the characters (0xC15B0): a character
// without a body is out of reach, as a last guard for the list above (a guest's explosion read one at 0x7C)
X86_EXTERN_FUNC(__imp__sub_000BE180);
X86_FUNC(sub_000BE180) {
    if (R32(kSessionUp) && (!Valid(ctx.eax, 0x158) || !Valid(R32(ctx.eax + 0x7C), kActorSize))) {
        LOG_FIRST(5, "netbots: a character without a body passed over (%08X)", ctx.eax);
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_000BE180(ctx, base);
}

// HitBounds(esi body): on the others a bot keeps the host's
X86_EXTERN_FUNC(__imp__sub_0009EAB0);
X86_FUNC(sub_0009EAB0) {
    uint32_t body = ctx.esi;
    __imp__sub_0009EAB0(ctx, base);
    if (!body || !InGame(2)) return;
    for (uint32_t slot = 0; slot < kActorSlots; slot++) {
        const Bounds& b = g_bounds[slot];
        if (b.body == body && BotBody(slot) == body && b.spawn == R32(body + 0x258)) {
            std::memcpy(At(body + 0x1A8), b.center, 12);
            std::memcpy(At(body + 0x1C0), b.radius, 12);
            break;
        }
    }
}

// Pose(body, advance): on the others a bot is posed where the host's animation is, not moved on; then its weapons
X86_EXTERN_FUNC(__imp__sub_0005FDC0);
X86_FUNC(sub_0005FDC0) {
    uint32_t body = R32(ctx.esp + 4);
    if (!InGame(2) || BotSlot(body) >= kActorSlots) {
        __imp__sub_0005FDC0(ctx, base);
        return;
    }
    uint32_t advance = ctx.esp + 8, saved = R32(advance);
    W32(advance, 0);
    __imp__sub_0005FDC0(ctx, base);
    W32(advance, saved);
    float health;
    std::memcpy(&health, At(body + 0x1CC), 4);
    if (health > 0 && (R32(body + 0x28) & 1)) {
        kernel::CallGuest(ctx, 0x1BE870, {body, 0});
        kernel::CallGuest(ctx, 0x1BE870, {body, 1});
    }
}
