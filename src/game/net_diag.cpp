// Network diagnostics: while this game is in a System Link game (its lobby or a match) or an online co-op session
// (net/lockstep.h), logs/net-host.log or logs/net-guest.log says what the network and the game do, for a look at the
// host's and a guest's side by side. The one before stays as net-host.1.log (net-guest.1.log).
//
// - events: the menus' and the session's state each time a part of it changes (the menu mode, the session, its role and
//   lobby state, the game type and map, the players here and in the match);
// - every second: the datagrams and bytes to and from each peer (xdk/xnet.h), the lockstep session's line;
// - every second in a level: the game's characters [0x7A3580] (count [0x74C37C]), each by its slot in the actor pool
//   [0x7356D8] (0xEA0 each; humans first, then bots or the level's AI), its body's place (+0x64) and health (+0x1CC):
//   a count and a hash each second, the whole list every 5;
// - in a System Link game each game sends its characters every second as a side datagram, and each logs how the
//   other's differ from its own: characters the other has and this one not, and those more than 2 units apart.
//   The other game must be a build with this too (its datagrams are heard: "the other's diagnostics are coming in").
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <process.h>

#include "core/log.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/hle.h"
#include "xdk/xnet.h"

namespace fs = std::filesystem;

namespace {

using kernel::R32;

constexpr uint32_t kSessionUp = 0x76B108, kRole = 0x76B140, kLobbyState = 0x76B1A4;  // role 1 host, 2 joined
constexpr uint32_t kInGame = 15;
constexpr uint32_t kMenuMode = 0x78AD2C, kMenuMap = 0x78AD4C, kArcadeMap = 0x78ADAC, kGameType = 0x78ADAE;
constexpr uint32_t kLevel = 0x79094C, kGameMode = 0x7DE455, kLocalPlayers = 0x5236C4, kLevelPlayers = 0x790950;
constexpr uint32_t kPlayConfig = 0x7DE400, kTick = 0x7DE318;
constexpr uint32_t kNetPlayers = 0x77728C, kNetPlayerSize = 0x124, kNetSlots = 16;  // +0: bit 0 a player there
constexpr uint32_t kCharacters = 0x7A3580, kCharacterCount = 0x74C37C, kRoom = 32;
constexpr uint32_t kActors = 0x7356D8, kActorSize = 0xEA0;
constexpr uint32_t kEvery = 60, kListEvery = 5;
constexpr float kFar = 2.0f;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
uint8_t Byte(uint32_t va) { return uint8_t(R32(va) & 0xFF); }
float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}

// ---- the log ----

FILE* g_file = nullptr;
std::vector<std::string> g_early;  // lines before this game's role is known
uint64_t g_frames = 0;
bool g_dirty = false;  // lines written since the last flush
auto g_start = std::chrono::steady_clock::now();

bool SystemLink() { return R32(kSessionUp) != 0; }
bool Network() {
    uint32_t mode = R32(kMenuMode);
    return lockstep::Active() || SystemLink() || (mode >= 0xB && mode <= 0xD);
}
// "host", "guest" or empty while it is not known
const char* Role() {
    if (lockstep::Active()) return lockstep::LocalPort() == 0 ? "host" : "guest";
    if (!SystemLink()) return "";
    uint32_t role = R32(kRole);
    return role == 1 ? "host" : role == 2 ? "guest" : "";
}

void Open(const char* role) {
    fs::path dir = fs::path(logx::Path()).parent_path();
    fs::path path = dir / (std::string("net-") + role + ".log");
    std::error_code ec;
    if (fs::exists(path, ec)) fs::rename(path, dir / (std::string("net-") + role + ".1.log"), ec);
    // the other game on this PC with the same role still has it open: this one writes beside it
    if (ec) path = dir / (std::string("net-") + role + "-" + std::to_string(_getpid()) + ".log");
    g_file = std::fopen(path.string().c_str(), "w");
    if (!g_file) return;
    LOGR("netdiag: %s", path.string().c_str());
    std::fprintf(g_file, "TSFP network diagnostics, this game the %s%s\n", role,
                 lockstep::Active() ? " of an online co-op session (lockstep)" : " of a System Link game");
    for (const std::string& line : g_early) std::fputs(line.c_str(), g_file);
    g_early.clear();
    std::fflush(g_file);
}

void Line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Line(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char line[2200];
    std::snprintf(line, sizeof line, "[%9.3f] f %llu tick %u | %s\n", t,
                  static_cast<unsigned long long>(lockstep::Active() ? lockstep::Frame() : g_frames), R32(kTick), msg);
    if (g_file) {
        std::fputs(line, g_file);
        g_dirty = true;
    } else if (g_early.size() < 400) {
        g_early.push_back(line);
    }
}

// ---- the state's events ----

struct Watch {
    const char* name;
    uint32_t (*get)();
    uint32_t last;
    bool seen;
};
uint32_t NetPlayers() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kNetSlots; i++)
        if (R32(kNetPlayers + i * kNetPlayerSize) & 1) mask |= 1u << i;
    return mask;
}
Watch g_watch[] = {
    {"menu mode", [] { return R32(kMenuMode); }},
    {"session up", [] { return R32(kSessionUp); }},
    {"role (1 host, 2 guest)", [] { return R32(kRole); }},
    {"lobby state (11 lobby, 15 in game)", [] { return R32(kLobbyState); }},
    {"game type (2 story)", [] { return uint32_t(Byte(kGameType)); }},
    {"arcade map (100 MapMaker)", [] { return uint32_t(Byte(kArcadeMap)); }},
    {"menu map", [] { return R32(kMenuMap); }},
    {"level", [] { return R32(kLevel); }},
    {"game mode (10 story)", [] { return uint32_t(Byte(kGameMode)); }},
    {"players on this PC", [] { return R32(kLocalPlayers); }},
    {"players in the level", [] { return R32(kLevelPlayers); }},
    {"network players (mask)", NetPlayers},
    {"match humans", [] { return uint32_t(Byte(kPlayConfig)); }},
    {"match bots", [] { return uint32_t(Byte(kPlayConfig + 2)); }},
    {"lockstep", [] { return uint32_t(lockstep::Active()); }},
};

void Events() {
    for (Watch& w : g_watch) {
        uint32_t v = w.get();
        if (w.seen && v == w.last) continue;
        Line("%s: %d%s", w.name, int32_t(v), w.seen ? "" : " (at start)");
        w.last = v, w.seen = true;
    }
    static std::string notice;
    std::string now = lockstep::Active() ? lockstep::Notice() : std::string();
    if (now != notice) Line("lockstep notice: %s", now.empty() ? "(none)" : now.c_str()), notice = now;
}

// ---- the characters ----

struct Character {
    int32_t slot;  // in the actor pool, -1 outside it
    uint32_t body;
    float pos[3], health;
};
struct Snapshot {
    uint32_t tick = 0, count = 0;
    Character c[kRoom]{};
};

Snapshot Characters() {
    Snapshot s;
    s.tick = R32(kTick);
    uint32_t count = std::min(R32(kCharacterCount), kRoom), pool = R32(kActors);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t actor = R32(kCharacters + i * 4);
        if (!Valid(actor, 4)) continue;
        Character& c = s.c[s.count++];
        c.slot = pool && actor >= pool && (actor - pool) % kActorSize == 0 && (actor - pool) / kActorSize < 64
                     ? int32_t((actor - pool) / kActorSize)
                     : -1;
        c.body = R32(actor);
        if (Valid(c.body, 0x1D0)) {
            for (int k = 0; k < 3; k++) c.pos[k] = F(c.body + 0x64 + k * 4);
            c.health = F(c.body + 0x1CC);
        }
    }
    std::sort(s.c, s.c + s.count, [](const Character& a, const Character& b) { return a.slot < b.slot; });
    return s;
}

// each place to half a unit: games agreeing hash alike
uint32_t Hash(const Snapshot& s) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    for (uint32_t i = 0; i < s.count; i++) {
        mix(uint32_t(s.c[i].slot));
        for (float p : s.c[i].pos) mix(uint32_t(int32_t(std::lround(p * 2))));
    }
    return h;
}

void LogList(const Snapshot& s) {
    for (uint32_t i = 0; i < s.count; i++) {
        const Character& c = s.c[i];
        Line("  slot %2d %s (%9.2f %9.2f %9.2f) health %.2f", c.slot, c.body ? "body" : "NO BODY", c.pos[0], c.pos[1],
             c.pos[2], c.health);
    }
}

// ---- the other game's characters (System Link) ----

constexpr uint32_t kTag = 0x47414944;  // "DIAG"
struct Packet {
    uint32_t tag, role, frames;
    Snapshot s;
};
struct {
    std::mutex lock;
    Packet p{};
    bool fresh = false, heard = false;
} g_other;

void Receive(const uint8_t* data, size_t len, uint32_t) {
    Packet p;
    if (len != sizeof p) return;
    std::memcpy(&p, data, sizeof p);
    if (p.tag != kTag || p.s.count > kRoom) return;
    std::lock_guard<std::mutex> lock(g_other.lock);
    g_other.p = p;
    g_other.fresh = true;
}
const bool g_receiving = (xnet::SideReceiver(Receive), true);

void Compare(const Snapshot& mine) {
    Packet p;
    bool first;
    {
        std::lock_guard<std::mutex> lock(g_other.lock);
        if (!g_other.fresh) return;
        p = g_other.p;
        g_other.fresh = false;
        first = !std::exchange(g_other.heard, true);
    }
    const char* who = p.role == 1 ? "host" : p.role == 2 ? "guest" : "other";
    if (first) Line("the other's diagnostics are coming in (the %s)", who);
    std::string only_there, only_here, apart;
    char part[96];
    for (uint32_t i = 0; i < p.s.count; i++) {
        const Character& o = p.s.c[i];
        const Character* m = nullptr;
        for (uint32_t j = 0; j < mine.count; j++)
            if (mine.c[j].slot == o.slot) m = &mine.c[j];
        if (!m) {
            std::snprintf(part, sizeof part, " %d", o.slot);
            only_there += part;
            continue;
        }
        float d = std::sqrt((o.pos[0] - m->pos[0]) * (o.pos[0] - m->pos[0]) + (o.pos[1] - m->pos[1]) * (o.pos[1] - m->pos[1]) +
                            (o.pos[2] - m->pos[2]) * (o.pos[2] - m->pos[2]));
        if (d > kFar || (o.body != 0) != (m->body != 0)) {
            std::snprintf(part, sizeof part, " %d:%.1f%s", o.slot, d, (o.body != 0) != (m->body != 0) ? "(body)" : "");
            apart += part;
        }
    }
    for (uint32_t j = 0; j < mine.count; j++) {
        bool there = false;
        for (uint32_t i = 0; i < p.s.count; i++) there |= p.s.c[i].slot == mine.c[j].slot;
        if (!there) std::snprintf(part, sizeof part, " %d", mine.c[j].slot), only_here += part;
    }
    int32_t lag = int32_t(mine.tick - p.s.tick);
    if (only_there.empty() && only_here.empty() && apart.empty()) {
        Line("vs the %s: %u characters agree (its tick %u, %d behind here)", who, p.s.count, p.s.tick, lag);
        return;
    }
    Line("vs the %s: it has %u characters, this game %u (its tick %u, %d behind here)%s%s%s%s%s%s", who, p.s.count,
         mine.count, p.s.tick, lag, only_there.empty() ? "" : "; only there:", only_there.c_str(),
         only_here.empty() ? "" : "; only here:", only_here.c_str(), apart.empty() ? "" : "; apart (slot:units):",
         apart.c_str());
}

void Second();

}  // namespace

namespace game {

// the game's frame starts (main thread, game/coop_online.cpp's frame clock)
void NetDiagFrame() {
    if (!g_file && !Network()) {
        g_frames = 0;
        return;
    }
    g_frames++;
    if (!g_file)
        if (const char* role = Role(); *role) Open(role);
    Events();
    if (g_frames % kEvery == 0 && Network()) Second();  // out of the network the events alone
    if (g_file && std::exchange(g_dirty, false)) std::fflush(g_file);
}

}  // namespace game

namespace {

void Second() {
    Line("traffic: %s", xnet::TrafficLine().c_str());
    if (lockstep::Active()) Line("lockstep: %s", lockstep::Status().c_str());
    bool in_level = lockstep::Active() ? R32(kCharacterCount) != 0 : SystemLink() && R32(kLobbyState) == kInGame;
    if (in_level) {
        Snapshot s = Characters();
        Line("characters %u hash %08X", s.count, Hash(s));
        if (g_frames % (kEvery * kListEvery) == 0) LogList(s);
        if (SystemLink() && !lockstep::Active()) {
            Packet p{kTag, R32(kRole), uint32_t(g_frames), s};
            xnet::SideSend(&p, sizeof p);
            Compare(s);
        }
    }
}

}  // namespace
