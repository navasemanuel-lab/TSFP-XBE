// Discord Rich Presence, after the TimeSplitters Future Perfect PC port (TheDoffman/timesplitters-future-perfect-pc,
// docs/discord-presence.md): the profile shows what is being played, Story's mission, difficulty, single player or
// co-op, Arcade's rules, map, players and bots, League and Challenge events, MapMaker (building a map, testing it,
// playing one), System Link and online games (a join address set), menus and the pause, with the time since the game
// started. It goes to the Discord desktop app through its local pipe (\\.\pipe\discord-ipc-N), no SDK: the
// handshake with the application id, then SET_ACTIVITY when the text changes, at most every 15 seconds. Without
// Discord running nothing happens, a worker thread tries again every 5 seconds.
//
// The game is read once a second on its own thread (from the camera build, game/fov.cpp) into plain values; the
// worker never reads guest memory. Texts come from the game's loaded language tables. Settings: discord (on/off) and
// discord_app_id (the Discord application whose name is shown, this port's own), discord_image (the picture
// beside it: one of that application's art assets by name, or an https link).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

// what the game is doing, copied on its thread
struct Snapshot {
    int32_t scene = 101;
    uint32_t flags = 0, menu_mode = 0, mode = 0;
    int network = 0, role = 0;  // network 1 System Link, 2 online (joined through an address past the LAN)
    bool editing = false;       // in MapMaker's editor
    bool paused = false, lobby = false;
    std::string mission, map, rules, event;
    uint32_t humans = 0, bots = 0, capacity = 0, difficulty = 0;
};

// the game's addresses (US)
constexpr uint32_t kScene = 0x79094C;        // 0-100 a level, over 100 the front end
constexpr uint32_t kFlags = 0x7DE458;        // 0x20000000 a cutscene
constexpr uint32_t kMenuMode = 0x78AD2C, kMode = 0x78AD20;
constexpr uint32_t kPauseStates = 0x761C48;  // the first local player's, bit 0 paused
constexpr uint32_t kSession = 0x76B0FC, kSessionUp = 0x76B108, kRole = 0x76B140, kLobbyState = 0x76B1A4;
constexpr uint32_t kTextsLevel = 0x6B7B10, kTextsCommon = 0x6B7B08;  // sub_0003D7B0: ids from 0xD87 the common table
constexpr uint32_t kPlayConfig = 0x7DE400, kMenuConfig = 0x78AD58;   // +0 humans, +2 bots, +0x54 map, +0x56 rules,
                                                                    // +0x57 difficulty, +0x70 event
constexpr uint32_t kStoryRows = 0x4D1AC0;     // 13 of 0x44: +2 title id, +6 scene
constexpr uint32_t kStoryMission = 0x78AD48, kStoryDifficulty = 0x78AD54, kProfiles = 0x5236C4;
constexpr uint32_t kRulesRows = 0x4D1F90;     // 20 of 0x20: +0 name id
constexpr uint32_t kMapRows = 0x4D2390;       // 17 of 0x3C: +0 scene, -8 name id, +0xC.. capacities
// event i's row: [0x510788 + i*4] for the 21 Challenges, [0x513D04 + i*4] for the Leagues (i from 21); +0 name id, +8
// scene
constexpr uint32_t kChallenges = 0x510788, kLeagues = 0x513D04;
constexpr uint32_t kNetPlayers = 0x77728C;    // 16 of 0x124, bit 0 present
constexpr uint32_t kMapMakerMode = 8;         // the menu mode MapMaker's Create and Load set
constexpr int32_t kMapMakerScene = 100;       // a MapMaker map played

ULONGLONG g_editor_seen = 0;  // MapMaker's cursor last drawn

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
uint32_t R32(uint32_t va) { return Valid(va, 4) ? kernel::R32(va) : 0; }
uint32_t R16(uint32_t va) { return Valid(va, 2) ? *static_cast<const uint16_t*>(kernel::HostPtr(va)) : 0; }
uint32_t R8(uint32_t va) { return Valid(va, 1) ? *static_cast<const uint8_t*>(kernel::HostPtr(va)) : 0; }

// a game text (Latin-1) as UTF-8; empty when missing or not plain text
std::string Text(uint32_t id) {
    if (!id || id > 0x2000 || !R32(kTextsCommon)) return {};
    uint32_t table = R32(id >= 0xD87 ? kTextsCommon : kTextsLevel);
    if (id >= 0xD87) id -= 0xD87;
    uint32_t p = table ? R32(table + id * 4) : 0;
    if (!p) return {};
    std::string out;
    for (uint32_t i = 0; i < 192; i++) {
        if (!Valid(p + i, 1)) return {};
        unsigned char c = *static_cast<const unsigned char*>(kernel::HostPtr(p + i));
        if (!c) return out;
        if (c < 32 || (c >= 127 && c < 160)) return {};
        if (c >= 128) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 63));
        } else {
            out += char(c);
        }
    }
    return {};
}

// League and Challenge titles are front end texts, unloaded in play: kept while the front end's table is there
uint32_t g_event_table = 0;
std::string g_events[48];

Snapshot Sample() {
    Snapshot s;
    s.scene = int32_t(R32(kScene));
    s.flags = R32(kFlags);
    s.menu_mode = R32(kMenuMode);
    s.mode = R32(kMode);
    bool active = s.scene >= 0 && s.scene <= 100;
    uint32_t mode = active ? s.mode : s.menu_mode;
    uint32_t pause = R32(kPauseStates);
    s.paused = active && pause && (R32(pause) & 1);
    if (R32(kSession) && R32(kSessionUp)) {  // a System Link session
        s.network = app::Get().join_ip.empty() ? 1 : 2;
        s.role = int(R32(kRole));
        s.lobby = R32(kLobbyState) == 11;
    }
    s.editing = s.menu_mode == kMapMakerMode && GetTickCount64() - g_editor_seen < 2000;
    if (s.scene == 101) {
        uint32_t table = R32(kTextsLevel);
        if (table && table != g_event_table) {
            for (uint32_t i = 0; i < 48; i++) {
                uint32_t row = R32((i < 21 ? kChallenges : kLeagues) + i * 4);
                g_events[i] = row ? Text(R16(row)) : std::string();
            }
            g_event_table = table;
        }
    }
    if (!active && !(s.network && s.lobby && mode >= 11 && mode <= 13)) return s;
    uint32_t config = active ? kPlayConfig : kMenuConfig;
    s.humans = R8(config);
    s.bots = R8(config + 2);
    if (s.network) {
        s.humans = 0;
        for (uint32_t i = 0; i < 16; i++)
            if (R32(kNetPlayers + i * 0x124) & 1) s.humans++;
    }
    if (s.humans > 16 || s.bots > 10 || s.humans + s.bots > 16) s.humans = s.bots = 0;
    if (!s.network && (mode == 4 || mode == 14 || (s.flags & 0x20000000))) {
        uint32_t d = R8(config + 0x57);
        s.difficulty = d < 3 ? d + 1 : 0;
        for (uint32_t i = 0; i < 13; i++) {
            uint32_t row = kStoryRows + i * 0x44;
            if (int32_t(int16_t(R16(row + 6))) == s.scene) {
                s.mission = Text(R16(row + 2));
                break;
            }
        }
        // a cutscene switches the scene: the chosen mission is still the one played
        if ((s.flags & 0x20000000) && (mode == 4 || mode == 14)) {
            uint32_t mission = R32(kStoryMission), chosen = R8(kStoryDifficulty);
            if (mission < 13) s.mission = Text(R16(kStoryRows + 2 + mission * 0x44));
            s.difficulty = chosen < 3 ? chosen + 1 : 0;
            s.humans = R8(kProfiles);
            if (s.humans > 2) s.humans = 0;
        }
        return s;
    }
    if (!s.network && mode != 5 && mode != 6 && mode != 7 && mode != 9) return s;
    uint32_t rules = R8(config + 0x56);
    if (rules < 20) s.rules = Text(R16(kRulesRows + rules * 0x20));
    for (uint32_t i = 0; i < 17; i++) {
        uint32_t row = kMapRows + i * 0x3C;
        int32_t scene = int32_t(R32(row)), target = active ? s.scene : int8_t(R8(config + 0x54));
        if (scene != target) continue;
        s.map = scene == 100 ? "MapMaker" : Text(R32(row - 8));
        if (s.network) {
            uint32_t column = mode == 11 ? 2 : mode == 13 ? 3 : 0;
            s.capacity = R32(row + 0xC + column * 4);
            if (s.capacity > 16 || s.capacity < s.humans + s.bots) s.capacity = 0;
        }
        break;
    }
    if (!s.network && (mode == 6 || mode == 7)) {
        uint32_t event = R32(config + 0x70);
        if (event < 48 && (mode == 7) == (event < 21)) {
            uint32_t row = R32((event < 21 ? kChallenges : kLeagues) + event * 4);
            if (row && int32_t(R32(row + 8)) == s.scene) s.event = g_events[event];
        }
    }
    return s;
}

// the two lines shown, each under 128 bytes
struct Activity {
    std::string details, state;
    bool operator==(const Activity&) const = default;
};

void Append(std::string& out, const std::string& text, const char* separator) {
    if (text.empty()) return;
    std::string add = out.empty() ? text : separator + text;
    size_t room = out.size() < 127 ? 127 - out.size() : 0;
    if (add.size() > room) {
        size_t n = room;
        while (n && (static_cast<unsigned char>(add[n]) & 0xC0) == 0x80) n--;
        add.resize(n);
    }
    out += add;
}

Activity Describe(const Snapshot& s) {
    Activity a;
    bool active = s.scene >= 0 && s.scene <= 100;
    bool story = !s.network && (s.mode == 4 || s.mode == 14 || (s.flags & 0x20000000));
    const char* phase = s.paused ? "Paused" : (s.flags & 0x20000000) ? "Watching a cutscene" : "In game";
    if (s.editing) {  // its cursor shows: the editor, whatever scene it has loaded
        a.details = "MapMaker";
        a.state = "Building a map";
        return a;
    }
    if (active && s.scene == kMapMakerScene && s.menu_mode == kMapMakerMode && !s.network) {
        a.details = "MapMaker";
        a.state = std::string("Testing a map | ") + phase;
        return a;
    }
    if (!active && !(s.network && s.lobby && s.menu_mode >= 11 && s.menu_mode <= 13)) {
        const char* menu = "Main menu";
        switch (s.menu_mode) {
            case 4: menu = "Story"; break;
            case 5: case 9: menu = "Arcade Custom"; break;
            case 6: menu = "Arcade League"; break;
            case 7: menu = "Challenge"; break;
            case 8: menu = "MapMaker"; break;
            case 11: case 12: case 13:
                menu = s.network == 2 ? "Online - Browsing matches"
                       : s.network   ? "System Link - Browsing matches"
                                     : "Multiplayer menu";
                break;
        }
        a.details = "In menus";
        a.state = menu;
        return a;
    }
    const char* mode = "Playing";
    if (s.network) mode = s.network == 2 ? "Online" : "System Link";
    else if (s.scene == kMapMakerScene) mode = "MapMaker";
    else if (story) mode = "Story";
    else if (s.mode == 7) mode = "Challenge";
    else if (s.mode == 6) mode = "Arcade League";
    else if (s.mode == 5 || s.mode == 9) mode = "Arcade Custom";
    Append(a.details, mode, "");
    if (story) {
        static const char* const kDifficulty[] = {"", "Easy", "Normal", "Hard"};
        Append(a.details, s.mission, " - ");
        if (s.difficulty <= 3) Append(a.state, kDifficulty[s.difficulty], " | ");
        if (s.humans == 2) Append(a.state, "Co-op", " | ");
        else if (s.humans == 1) Append(a.state, "Single player", " | ");
        Append(a.state, phase, " | ");
        return a;
    }
    std::string population;
    if (s.humans >= 1 && s.humans <= 16 && s.bots <= 10 && s.humans + s.bots <= 16) {
        population = std::to_string(s.humans) + (s.humans == 1 ? " player" : " players");
        if (s.bots) population += " + " + std::to_string(s.bots) + (s.bots == 1 ? " bot" : " bots");
        if (s.network && s.capacity >= s.humans + s.bots && s.capacity <= 16)
            population += " / " + std::to_string(s.capacity);
    }
    if (!active) {
        Append(a.details, s.role == 1 ? "Hosting lobby" : s.role == 2 ? "Joined lobby" : "In lobby", " - ");
        Append(a.state, s.rules, " | ");
    } else {
        Append(a.details, s.event.empty() ? s.rules : s.event, " - ");
    }
    if (s.network || s.scene != kMapMakerScene) Append(a.state, s.map, " | ");
    Append(a.state, population, " | ");
    if (active) {
        if (s.network && s.role == 1) Append(a.state, "Hosting", " | ");
        Append(a.state, phase, " | ");
        if (!s.event.empty()) Append(a.state, s.rules, " | ");
    }
    if (a.state.empty()) a.state = "Waiting for players";
    return a;
}

std::string Quote(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (c < 32) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", c);
            out += u;
        } else {
            if (c == '"' || c == '\\') out += '\\';
            out += char(c);
        }
    }
    return out;
}

// the worker and the last sample it reads
std::mutex g_lock;
Activity g_latest{"In menus", "Main menu"};
HANDLE g_worker = nullptr, g_stop = nullptr;
std::string g_app_id, g_image;

bool Stopping(DWORD ms) { return WaitForSingleObject(g_stop, ms) != WAIT_TIMEOUT; }

// a whole read or write, given up after 250 ms or when the game closes
bool Transfer(HANDLE pipe, void* buffer, DWORD bytes, bool writing) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    BOOL ok = writing ? WriteFile(pipe, buffer, bytes, &done, &ov) : ReadFile(pipe, buffer, bytes, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[2] = {g_stop, ov.hEvent};
        if (WaitForMultipleObjects(2, waits, FALSE, 250) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &done, TRUE);
            ok = FALSE;
        } else {
            ok = GetOverlappedResult(pipe, &ov, &done, FALSE);
        }
    }
    CloseHandle(ov.hEvent);
    return ok && done == bytes;
}

bool Send(HANDLE pipe, uint32_t opcode, const std::string& json) {
    std::vector<char> frame(8 + json.size());
    uint32_t length = uint32_t(json.size());
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, json.data(), json.size());
    return Transfer(pipe, frame.data(), DWORD(frame.size()), true);
}

HANDLE OpenPipe() {
    for (int i = 0; i < 10; i++) {
        wchar_t name[64];
        std::swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

// a top-level "key":"value" in a reply
bool Field(const std::string& json, const char* key, const char* value) {
    return json.find(std::string("\"") + key + "\":\"" + value + "\"") != std::string::npos;
}

// the picture and its hover text
std::string Assets() {
    if (g_image.empty()) return {};
    return ",\"assets\":{\"large_image\":\"" + Quote(g_image) + "\",\"large_text\":\"TimeSplitters Future Perfect\"}";
}

std::string ActivityJson(const Activity* a, int64_t started, unsigned nonce) {
    std::string pid = std::to_string(GetCurrentProcessId()), n = std::to_string(nonce);
    if (!a) return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + pid + ",\"activity\":null},\"nonce\":\"" + n + "\"}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + pid + ",\"activity\":{\"type\":0,\"details\":\"" +
           Quote(a->details) + "\",\"state\":\"" + Quote(a->state) + "\",\"timestamps\":{\"start\":" +
           std::to_string(started) + "}" + Assets() + "}},\"nonce\":\"" + n + "\"}";
}

DWORD WINAPI Run(void*) {
    constexpr ULONGLONG kInterval = 15000, kRetry = 5000;
    constexpr uint32_t kMaxFrame = 65536;
    int64_t started = int64_t(std::time(nullptr));
    unsigned nonce = 0;
    bool told_ready = false, told_error = false;
    std::vector<char> in;
    while (!Stopping(0)) {
        HANDLE pipe = OpenPipe();
        if (pipe != INVALID_HANDLE_VALUE) {
            bool connected = Send(pipe, 0, "{\"v\":1,\"client_id\":\"" + g_app_id + "\"}"), ready = false, sent = false;
            ULONGLONG opened = GetTickCount64(), sent_at = 0;
            Activity previous;
            in.clear();
            while (connected && !Stopping(100)) {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
                if (available) {
                    size_t used = in.size();
                    DWORD take = std::min<DWORD>(available, kMaxFrame + 8 - DWORD(used));
                    in.resize(used + take);
                    if (!take || !Transfer(pipe, in.data() + used, take, false)) break;
                    while (in.size() >= 8) {
                        uint32_t opcode, length;
                        std::memcpy(&opcode, in.data(), 4);
                        std::memcpy(&length, in.data() + 4, 4);
                        if (length > kMaxFrame) {
                            connected = false;
                            break;
                        }
                        if (in.size() < 8 + length) break;
                        std::string json(in.data() + 8, length);
                        if (opcode == 2) {
                            connected = false;
                        } else if (opcode == 3) {  // ping: pong with its data
                            connected = Send(pipe, 4, json);
                        } else if (opcode == 1) {
                            if (Field(json, "evt", "ERROR")) {
                                if (!told_error) LOGR("discord: presence refused, check discord_app_id");
                                told_error = true;
                                connected = false;
                            } else if (Field(json, "cmd", "DISPATCH") && Field(json, "evt", "READY")) {
                                ready = true;
                                if (!told_ready) LOGR("discord: connected to the Discord app");
                                told_ready = true;
                            }
                        } else if (opcode != 4) {
                            connected = false;
                        }
                        in.erase(in.begin(), in.begin() + 8 + length);
                        if (!connected) break;
                    }
                }
                if (!ready && GetTickCount64() - opened > 10000) break;
                if (connected && ready && (!sent || GetTickCount64() - sent_at >= kInterval)) {
                    Activity a;
                    {
                        std::lock_guard<std::mutex> l(g_lock);
                        a = g_latest;
                    }
                    if (!sent || !(a == previous)) {
                        connected = Send(pipe, 1, ActivityJson(&a, started, ++nonce));
                        if (connected) {
                            previous = a;
                            sent_at = GetTickCount64();
                            sent = true;
                            LOGR("discord: %s / %s", a.details.c_str(), a.state.c_str());
                        }
                    }
                }
            }
            if (ready && Stopping(0)) Send(pipe, 1, ActivityJson(nullptr, started, ++nonce));
            CloseHandle(pipe);
        }
        if (Stopping(kRetry)) break;
    }
    return 0;
}

}  // namespace

namespace game {

// a game text by its id (Latin-1 as UTF-8), empty when it is not loaded
std::string GameText(uint32_t id) { return Text(id); }

// after the settings load: the worker, when presence is on and the id is an application's (17-20 digits)
void StartDiscord() {
    if (g_worker || !Opt().discord) return;
    g_app_id = Opt().discord_app_id;
    g_image = Opt().discord_image;
    bool digits = g_app_id.size() >= 17 && g_app_id.size() <= 20 && g_app_id[0] != '0' &&
                  g_app_id.find_first_not_of("0123456789") == std::string::npos;
    if (!digits) {
        LOGR("discord: discord_app_id is not an application id, presence off");
        return;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return;
    g_worker = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    if (!g_worker) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
}

// the window closing: the presence cleared
void StopDiscord() {
    if (!g_worker) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_worker, 2000);
    CloseHandle(g_worker);
    CloseHandle(g_stop);
    g_worker = g_stop = nullptr;
}

// MapMaker's editor drew its cursor (game/mapmaker.cpp)
void MapMakerEditorSeen() { g_editor_seen = GetTickCount64(); }

// on the game's thread, often: the game read once a second
void SampleDiscord() {
    static ULONGLONG last = 0;
    if (!g_worker) return;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    Activity a = Describe(Sample());
    std::lock_guard<std::mutex> l(g_lock);
    g_latest = a;
}

}  // namespace game
