// Online Story co-op over System Link, without lockstep: after the TimeSplitters Future Perfect PC port's
// online-story-coop branch (TheDoffman/timesplitters-future-perfect-pc, GPLv3, src/pc_story_*.inc), whose findings on
// this game these are. Every game runs its own world and the host owns it, as in System Link's Arcade. This part takes
// the players into a Story mission together; the NPCs, shots and the rest the host owns follow in their own parts.
//
// The entry: the main menu's first row (Story, built by 0x2D0BE0 at 0x2D0C8E with id 0 and the menu's own callback)
// opens a chooser: Offline Story is the game's own Story menu (descriptor 0x5238E0); Host and Join Online Story start
// the game again in a lockstep session (net/lockstep.h, game/coop_online.cpp: both PCs run the same split screen
// co-op), joining the address of the settings panel's Co-op page. In a session the chooser says Story and Leave
// Online Story. The System Link way below (g_online) is off: nothing marks a session as Story's any more.
//
// The lobby (descriptors 0x4E99C4 host, 0x4E99D4 joined, ticks 0x1A8D30 and 0x1A8F30): Y on the host opens Story's
// own mission list (0x5238E0, built by 0x2CB090 while the menu mode [0x78AD2C] reads Story's 4) and then a difficulty
// menu of its own; X opens the players' progress (0x523940). The joined games are made ready (0x359EB0(0, 1)). The
// lobby's header row (id 0x145 / 0x14B, drawn by 0x197890) shows the mission as Story's list does (0x2CA9F0) with its
// year, difficulty and players under it, its details (0x1986A0 from 0x199650) the mission's text. The host's settings
// [0x78AD58] carry the choice as the lobby sends them (0x354240): +0x54 the scene, +0x55 10 and +0x56 2 (Story), +0x57
// the difficulty, every player's character 12.
//
// The load: the settings sent (0x356490) and loaded (0x658D0) are made Story's: the host's are built by Story's own
// first stage (0x65940, with the menu reading Story's mode, mission and difficulty), its header kept (the players, the
// local one, no bots). After the intro the host builds the next load (0x65C70) and sends it with a new generation
// (0x352640/0x352650); the joined games wait for it. The ready exchange (0x357540) is done without Arcade's pad loop.
// A Story scene's records (0x6B100) are taken as co-op's, those for two kept for up to four. Each player's context
// still waiting when the level starts is started at Story's own start (0xD50E0); in the first mission's opening
// (scene 2) players 1 and 2 take its two starts (pads 0x1F40, 0x1F62) and players 3 and 4 two made beside the first
// (0x7F02, 0x7F03, read through 0x9A420/0x9A4C0/0x9A510). Every human is on team 1 (0xD7550, 0xD7570). The network
// tick leaves Story's characters on the character list (0x15BCC0 from 0x355EA4), and its player payloads (0x35BD70,
// 0x35DB60) wait until every player has a body.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "common/options.h"
#include "common/panel.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "net/lockstep.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // cpu/dispatch.cpp: a guest address that calls host code
}

namespace {

using kernel::R32;
using kernel::W32;

constexpr uint32_t kSessionUp = 0x76B108, kRole = 0x76B140, kLobbyState = 0x76B1A4;  // role 1 host, 2 joined
constexpr uint32_t kHostLobby = 11, kJoinedLobby = 12, kInGame = 15;
constexpr uint32_t kNetPlayers = 0x77728C, kNetPlayerSize = 0x124;  // +0: bit 0 a player there
constexpr uint32_t kLocalNode = 0x774028;                           // +0x2FA: this game ready
constexpr uint32_t kMenuSetup = 0x78AD20, kMenuMode = 0x78AD2C, kMenuMission = 0x78AD48, kMenuStage = 0x78AD50;
constexpr uint32_t kMenuDifficulty = 0x78AD54, kMenuConfig = 0x78AD58, kStoryMode = 4;
constexpr uint32_t kPlayConfig = 0x7DE400, kGameType = 0x7DE455, kGameFlags = 0x7DE458, kResult = 0x7DE45C;
constexpr uint32_t kEndType = 0x7DE30C, kScene = 0x79094C, kPlayers = 0x790950;
constexpr uint32_t kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584, kActors = 0x7356D8, kActorSize = 0xEA0;
constexpr uint32_t kMissions = 0x4D1AC0, kMissionSize = 0x44, kMissionCount = 13;  // +0 year text, +6 scene, +0x10 text
constexpr uint32_t kStoryList = 0x5238E0, kStoryHeader = 0x5238C0, kProgress = 0x523940;
constexpr uint32_t kLobbyHost = 0x4E99C4, kLobbyJoined = 0x4E99D4;
constexpr uint32_t kButtons = 0x6B94D8;  // the network menus' presses: 0x40 Y, 0x10 X
constexpr uint32_t kStoryFlags = 0x00080008, kMovieFlag = 0x20000000;
constexpr uint32_t kTextScaleX = 0x7E7270, kTextScaleY = 0x7E7280;
constexpr uint32_t kLocalPlayers = 0x5236C4;
constexpr uint32_t kOpeningScene = 2, kStart1 = 0x1F40, kStart2 = 0x1F62, kExtraStart = 0x7F00;

constexpr uint32_t kShowMenu = 0x79B70, kMenuTitle = 0x75B70, kMenuLayout = 0x71B10, kMenuGutter = 0x71BE0;
constexpr uint32_t kMenuStyle = 0x71C50, kAddRow = 0x79D60, kFinishMenu = 0x771D0, kSelectRow = 0x770E0;
constexpr uint32_t kChosen = 0x18C;
constexpr uint32_t kText = 0x3D7B0, kFont = 0x3FA10, kApplyScale = 0x3F8D0, kMeasure = 0x3FCF0, kWrap = 0x40F00;
constexpr uint32_t kDrawText = 0x41DC0, kFade = 0x154FE0;

bool g_online = false;  // this game's System Link session is Story's
uint32_t g_mission = 0, g_difficulty = 1;
uint32_t g_options = 0;  // the host's lobby: 1 the mission list open, 2 the difficulty menu
uint32_t g_parent = 0, g_lobby = 0;
uint32_t g_stage = 0;  // the load: 0 the intro, 1 the level, 2 the ending, 3 the rewards
bool g_start_pending = false;
uint32_t g_body_menu = 0, g_body_rect = 0;

uint32_t g_storage = 0;  // the chooser's and the difficulty menu's descriptors and texts, scratch for the draws
uint32_t g_chooser = 0, g_difficulties = 0, g_scratch = 0, g_extra_pads = 0;
uint32_t g_open_cb = 0, g_build_cb = 0, g_select_cb = 0, g_back_cb = 0;
uint32_t g_diff_build_cb = 0, g_diff_select_cb = 0, g_diff_back_cb = 0, g_diff_draw_cb = 0;
uint32_t g_heading = 0, g_title = 0, g_local = 0, g_online_text = 0, g_back_text = 0, g_progress_text = 0;
uint32_t g_join_text = 0, g_session_story = 0, g_leave_text = 0;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
uint8_t* At(uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); }
float F(uint32_t va) {
    float f;
    std::memcpy(&f, At(va), 4);
    return f;
}
void SetF(uint32_t va, float f) { std::memcpy(At(va), &f, 4); }
uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
int16_t SceneOf(uint32_t mission) {
    int16_t scene;
    std::memcpy(&scene, At(kMissions + mission * kMissionSize + 6), 2);
    return scene;
}
uint32_t Role() { return R32(kRole); }
bool Session() { return g_online && R32(kSessionUp); }
bool LobbyHost() { return g_online && Role() == 1 && R32(kLobbyState) == kHostLobby; }
bool LobbyMember() { return LobbyHost() || (g_online && Role() == 2 && R32(kLobbyState) == kJoinedLobby); }
bool StoryScene() { return R32(kScene) < 100 && R32(kGameFlags) == kStoryFlags && At(kGameType)[0] == 10; }
uint32_t JoinedPlayers() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; i++) n += R32(kNetPlayers + i * kNetPlayerSize) & 1;
    return n;
}
uint32_t LastPlayer() {
    uint32_t last = 0;
    for (uint32_t i = 0; i < 16; i++)
        if (R32(kNetPlayers + i * kNetPlayerSize) & 1) last = i + 1;
    return last;
}

uint32_t Put(uint32_t& used, const char* s) {
    uint32_t va = g_storage + used, n = uint32_t(std::strlen(s) + 1);
    std::memcpy(At(va), s, n);
    used += n;
    return va;
}

void ChooserOpen(X86Context& ctx, uint8_t*);
void ChooserBuild(X86Context& ctx, uint8_t*);
void ChooserSelect(X86Context& ctx, uint8_t*);
void ChooserBack(X86Context& ctx, uint8_t*);
void DifficultyBuild(X86Context& ctx, uint8_t*);
void DifficultySelect(X86Context& ctx, uint8_t*);
void DifficultyBack(X86Context& ctx, uint8_t*);
void DifficultyDraw(X86Context& ctx, uint8_t*);

bool Prepare() {
    if (g_storage) return true;
    g_storage = mem::AllocSystem(0x400);
    if (!g_storage) return false;
    std::memset(At(g_storage), 0, 0x400);
    g_chooser = g_storage, g_difficulties = g_storage + 0x40;  // descriptors: +0 the build callback, the rest 0
    g_scratch = g_storage + 0x80;                              // 0x100: the draws' measures and caption
    g_extra_pads = g_storage + 0x180;                          // two starts of 32
    uint32_t used = 0x1C0;
    g_title = Put(used, "Story");
    g_local = Put(used, "Offline Story");
    g_online_text = Put(used, "Host Online Story");
    g_join_text = Put(used, "Join Online Story");
    g_session_story = Put(used, "Story");
    g_leave_text = Put(used, "Leave Online Story");
    g_back_text = Put(used, "Back");
    g_progress_text = Put(used, "Player Progress");
    g_heading = Put(used, "Select Difficulty");
    g_open_cb = dispatch::HostThunk(ChooserOpen);
    g_build_cb = dispatch::HostThunk(ChooserBuild);
    g_select_cb = dispatch::HostThunk(ChooserSelect);
    g_back_cb = dispatch::HostThunk(ChooserBack);
    g_diff_build_cb = dispatch::HostThunk(DifficultyBuild);
    g_diff_select_cb = dispatch::HostThunk(DifficultySelect);
    g_diff_back_cb = dispatch::HostThunk(DifficultyBack);
    g_diff_draw_cb = dispatch::HostThunk(DifficultyDraw);
    W32(g_chooser, g_build_cb);
    W32(g_difficulties, g_diff_build_cb);
    return true;
}

// ---- the chooser

uint32_t g_chooser_parent = 0, g_chooser_previous = 0;

void ChooserOpen(X86Context& ctx, uint8_t*) {
    uint32_t source = R32(ctx.esp + 4);
    if (Valid(source, 0x3C) && R32(source + 0x38) == 0 && Prepare()) {
        g_chooser_parent = R32(source + 0x2C);
        g_chooser_previous = R32(source + 4);
        kernel::CallGuest(ctx, kShowMenu, {g_chooser_parent, g_chooser});
    }
    kernel::Return(ctx, 0, 0);
}

void ChooserBuild(X86Context& ctx, uint8_t*) {
    uint32_t menu = R32(ctx.esp + 4);
    kernel::CallGuest(ctx, kMenuTitle, {0xA000, g_title, Bits(55.0f), 0, g_back_cb});
    kernel::CallGuest(ctx, kMenuLayout, {1, Bits(2.0f), 1, 1});
    kernel::CallGuest(ctx, kMenuStyle, {0x26200});
    if (lockstep::Active()) {
        kernel::CallGuest(ctx, kAddRow, {g_session_story, 0, 0, 0, g_select_cb, 0});
        kernel::CallGuest(ctx, kAddRow, {g_leave_text, 3, 0, 0, g_select_cb, 0});
    } else {
        kernel::CallGuest(ctx, kAddRow, {g_local, 0, 0, 0, g_select_cb, 0});
        kernel::CallGuest(ctx, kAddRow, {g_online_text, 1, 0, 0, g_select_cb, 0});
        kernel::CallGuest(ctx, kAddRow, {g_join_text, 2, 0, 0, g_select_cb, 0});
    }
    kernel::CallGuest(ctx, kFinishMenu, {menu});
    kernel::CallGuest(ctx, kSelectRow, {menu, 0});
    kernel::Return(ctx, 0, 0);
}

void ChooserBack(X86Context& ctx, uint8_t*) {
    kernel::CallGuest(ctx, kShowMenu, {g_chooser_parent, g_chooser_previous});
    kernel::Return(ctx, 0, 0);
}

void ChooserSelect(X86Context& ctx, uint8_t*) {
    uint32_t menu = R32(ctx.esp + 4);
    if (Valid(menu, kChosen + 4)) {
        uint32_t chosen = R32(menu + kChosen);
        if (chosen == 0) {
            g_online = false;
            kernel::CallGuest(ctx, kShowMenu, {g_chooser_parent, kStoryList});
            LOGR("storyonline: local Story");
        } else if (chosen == 1 && !lockstep::Active()) {
            const game::Options& o = game::Opt();
            app::Save();
            LOGR("storyonline: hosting online Story (lockstep) on port %u", o.net_port);
            lockstep::Restart("host:" + std::to_string(o.net_port), o.net_password);
        } else if (chosen == 2 && !lockstep::Active()) {
            const game::Options& o = game::Opt();
            if (o.net_address.empty()) {
                LOGR("storyonline: no address to join, the Co-op page opened");
                panel::ShowCoop();
            } else {
                app::Save();
                LOGR("storyonline: joining online Story (lockstep) at %s:%u", o.net_address.c_str(), o.net_port);
                lockstep::Restart("join:" + o.net_address + ":" + std::to_string(o.net_port), o.net_password);
            }
        } else if (chosen == 3 && lockstep::Active()) {
            LOGR("storyonline: leaving online Story");
            lockstep::Restart("", "");
        }
    }
    kernel::Return(ctx, 0, 0);
}

// ---- the lobby

// the host's choice into the settings the lobby sends
void LobbyConfig() {
    if (!LobbyHost() || !R32(kSessionUp) || g_mission >= kMissionCount || g_difficulty >= 3) return;
    uint8_t* config = At(kMenuConfig);
    int16_t scene = SceneOf(g_mission);
    if (scene < 0 || scene >= 100) return;
    config[0x54] = uint8_t(scene), config[0x55] = 10, config[0x56] = 2, config[0x57] = uint8_t(g_difficulty);
    config[2] = 0;
    for (uint32_t i = 0; i < 16; i++)
        if (R32(kNetPlayers + i * kNetPlayerSize) & 1) config[0x14 + i] = 12;
}

// a joined game: the host's choice from the settings it received
void ReceivedSelection() {
    if (LobbyHost()) return;
    const uint8_t* config = At(kMenuConfig);
    if (config[0x55] != 10 || config[0x56] != 2 || config[0x57] >= 3) return;
    for (uint32_t i = 0; i < kMissionCount; i++)
        if (SceneOf(i) == config[0x54]) {
            g_mission = i, g_difficulty = config[0x57];
            break;
        }
}

void Footer(uint32_t menu) {
    if (!Prepare()) return;
    if (LobbyHost()) {
        W32(menu + 0x14C, g_back_text);
        W32(menu + 0x15C, g_progress_text);
    } else {
        // footer records (action, caption, ...) at +0x128, 0x10 apart: B and X only, no Ready (the tick rewrites it)
        W32(menu + 0x128, 1), W32(menu + 0x12C, g_back_text);
        W32(menu + 0x138, 2), W32(menu + 0x13C, g_progress_text);
        W32(menu + 0x148, 0xFFFFFFFF), W32(menu + 0x158, 0xFFFFFFFF);
        W32(menu + 0x1BC, 0);
    }
}

void LobbyTick(X86Context& ctx, uint8_t* base, void (*original)(X86Context&, uint8_t*), uint32_t descriptor) {
    uint32_t menu = R32(ctx.esp + 4);
    if (!LobbyMember() || !Valid(menu, 0x1C4) || R32(menu + 4) != descriptor) return original(ctx, base);
    g_parent = R32(menu + 0x2C);
    g_lobby = R32(menu + 4);
    ReceivedSelection();
    if (!LobbyHost()) {
        uint32_t local = R32(kLocalNode);
        if (Valid(local, 0x300) && !At(local + 0x2FA)[0]) kernel::CallGuest(ctx, 0x359EB0, {0, 1});
        W32(menu + 0x1BC, 0);
    }
    // the tick would open Arcade's options (Y) and character select (X)
    uint32_t actions = R32(kButtons) & 0x50;
    W32(kButtons, R32(kButtons) & ~0x50u);
    original(ctx, base);
    W32(kButtons, (R32(kButtons) & ~0x50u) | actions);
    if (!LobbyMember() || !Valid(menu, 0x1C4) || R32(menu + 4) != g_lobby) return;
    Footer(menu);
    if ((actions & 0x40) && LobbyHost()) {
        g_options = 1;
        kernel::CallGuest(ctx, kShowMenu, {g_parent, kStoryList});
    } else if (actions & 0x10) {
        kernel::CallGuest(ctx, kShowMenu, {g_parent, kProgress});
    }
}

// the mission's year, difficulty and players under its name in the header
void LobbyStatus(X86Context& ctx, uint32_t menu, uint32_t rect) {
    if (g_mission >= kMissionCount || g_difficulty >= 3 || !Valid(menu, 0xF8) || !Valid(rect, 16) || !Prepare()) return;
    float l = F(rect), t = F(rect + 4), r = F(rect + 8), b = F(rect + 12);
    if (!(r > l + 64.0f && b > t + 16.0f)) return;
    uint32_t font = kernel::CallGuest(ctx, kFont, {3});
    uint16_t year_id;
    std::memcpy(&year_id, At(kMissions + g_mission * kMissionSize), 2);
    uint32_t year = kernel::CallGuest(ctx, kText, {year_id});
    static const char* const kDifficulty[] = {"Easy", "Normal", "Hard"};
    uint32_t caption = g_scratch + 16;
    std::snprintf(reinterpret_cast<char*>(At(caption)), 128, "%.31s  %s  %u/4 Players",
                  Valid(year, 32) ? reinterpret_cast<const char*>(At(year)) : "", kDifficulty[g_difficulty],
                  JoinedPlayers());
    uint32_t old_x = R32(kTextScaleX), old_y = R32(kTextScaleY);
    float sx = F(kTextScaleX) * 0.65f, sy = F(kTextScaleY) * 0.65f;
    SetF(kTextScaleX, sx), SetF(kTextScaleY, sy);
    kernel::CallGuest(ctx, kApplyScale, {});
    kernel::CallGuest(ctx, kMeasure, {g_scratch, g_scratch + 4, caption, 0});
    float width = F(g_scratch), height = F(g_scratch + 4);
    float available = (r - l) * 0.78f - 16.0f;
    if (std::isfinite(width) && width > available && available > 0.0f) {
        float fit = available / width;
        SetF(kTextScaleX, sx * fit), SetF(kTextScaleY, sy * fit);
        width = available, height *= fit;
        kernel::CallGuest(ctx, kApplyScale, {});
    }
    if (!std::isfinite(width) || width < 0.0f) width = 0.0f;
    if (!std::isfinite(height) || height < 0.0f) height = 12.0f;
    float title = 20.0f;
    float x = l + 8.0f + (available - width) * 0.5f;
    float y = t + ((b - t) - (title + height + 4.0f)) * 0.5f + title + 4.0f;
    uint32_t colour = kernel::CallGuest(ctx, kFade, {0x7F7F7F7F, R32(menu + 0xF4)});
    kernel::CallGuest(ctx, kDrawText, {Bits(x), Bits(y), caption, colour});
    W32(kTextScaleX, old_x), W32(kTextScaleY, old_y);
    kernel::CallGuest(ctx, kApplyScale, {});
    kernel::CallGuest(ctx, kFont, {font});
}

// ---- the difficulty menu

void DifficultyBuild(X86Context& ctx, uint8_t*) {
    uint32_t menu = R32(ctx.esp + 4);
    kernel::CallGuest(ctx, kMenuTitle, {0x0800A000, g_heading, Bits(42.0f), 0, g_diff_back_cb});
    kernel::CallGuest(ctx, kMenuLayout, {1, Bits(2.0f), 1, 1});
    kernel::CallGuest(ctx, kMenuGutter, {1, Bits(9.0f), 0});  // Story's difficulty rows leave room for the trophy
    kernel::CallGuest(ctx, kMenuStyle, {0x22200});
    for (uint32_t i = 0; i < 3; i++)
        kernel::CallGuest(ctx, kAddRow,
                          {kernel::CallGuest(ctx, kText, {0x3D5 + i}), i, 0, 0, g_diff_select_cb, g_diff_draw_cb});
    kernel::CallGuest(ctx, kFinishMenu, {menu});
    kernel::CallGuest(ctx, kSelectRow, {menu, g_difficulty < 3 ? g_difficulty : 0});
    kernel::Return(ctx, 0, 0);
}

// (menu, row, rect): Story's own difficulty row with its trophy, for the chosen mission
void DifficultyDraw(X86Context& ctx, uint8_t*) {
    uint32_t mission = R32(kMenuMission), players = R32(kLocalPlayers);
    W32(kMenuMission, g_mission), W32(kLocalPlayers, 2);
    kernel::CallGuest(ctx, 0x2CAD80, {R32(ctx.esp + 4), R32(ctx.esp + 8), R32(ctx.esp + 12)});
    W32(kMenuMission, mission), W32(kLocalPlayers, players);
    kernel::Return(ctx, 0, 0);
}

void DifficultySelect(X86Context& ctx, uint8_t*) {
    uint32_t menu = R32(ctx.esp + 4);
    if (Valid(menu, kChosen + 4) && R32(menu + kChosen) < 3) {
        g_difficulty = R32(menu + kChosen);
        g_options = 0;
        LOGR("storyonline: mission %u, difficulty %u", g_mission, g_difficulty);
        kernel::CallGuest(ctx, kShowMenu, {g_parent, g_lobby});
    }
    kernel::Return(ctx, 0, 0);
}

void DifficultyBack(X86Context& ctx, uint8_t*) {
    g_options = 1;
    kernel::CallGuest(ctx, kShowMenu, {g_parent, kStoryList});
    kernel::Return(ctx, 0, 0);
}

// ---- the load

bool OpeningStarts() {
    uint32_t role = Role(), count = R32(kPlayers);
    return Session() && R32(kLobbyState) == kInGame && (role == 1 || role == 2) && R32(kScene) == kOpeningScene &&
           R32(kGameFlags) == kStoryFlags && At(kGameType)[0] == 10 && count >= 1 && count <= 4;
}

bool HumanTeamScope(uint32_t actor) {
    uint32_t pool = R32(kActors), phase = R32(kLobbyState), role = Role();
    uint32_t humans = At(kPlayConfig)[0];
    return Session() && (phase == 10 || phase == 14 || phase == 15) && (role == 1 || role == 2) && StoryScene() &&
           humans && humans <= 4 && pool && Valid(actor, kActorSize) && R32(actor + 8) < humans &&
           actor == pool + R32(actor + 8) * kActorSize;
}

// once a human's body and context are there, on team 1 as every human in Story
void HumanTeams(X86Context& ctx) {
    uint32_t contexts = R32(kPlayerArray), actors = R32(kActors), count = R32(kPlayers);
    if (!count || count > 4 || !contexts || !actors || count != At(kPlayConfig)[0]) return;
    for (uint32_t slot = 0; slot < count; slot++) {
        uint32_t context = contexts + slot * kPlayerSize, actor = actors + slot * kActorSize;
        if (!Valid(context, 0x118) || !HumanTeamScope(actor)) continue;
        uint32_t body = R32(actor);
        if (!Valid(body, 0x154) || R32(context) != slot || R32(context + 0x14) != body || R32(body + 0x7C) != actor ||
            R32(body + 0x150) != context || R32(actor + 0x84) == 1)
            continue;
        kernel::CallGuest(ctx, 0xD7550, {actor, 1});
    }
}

}  // namespace

namespace game {

bool StoryOnline() { return Session(); }
bool StoryOnlineOpening() { return OpeningStarts(); }
// in a Story level of a session: its NPCs are the host's (net_bots.cpp)
bool StoryOnlineInLevel() {
    uint32_t role = Role();
    return Session() && (role == 1 || role == 2) && R32(kLobbyState) == kInGame && StoryScene();
}

// the main menu's rows (0x79D60, video_menu.cpp): its Story row opens the chooser; the main menu built leaves online
bool StoryOnlineRow(X86Context& ctx) {
    if (R32(ctx.esp) != 0x2D0C8E || R32(ctx.esp + 8) != 0 || R32(ctx.esp + 20) != 0) return false;
    if (g_online) LOGR("storyonline: back at the main menu, online Story off");
    g_online = false;
    if (!Prepare()) return false;
    W32(ctx.esp + 20, g_open_cb);
    return true;
}

// the lobby's header row (0x197890, net_bots.cpp): (menu, item, rect)
bool StoryOnlineLobbyHeader(X86Context& ctx) {
    uint32_t menu = R32(ctx.esp + 4), item = R32(ctx.esp + 8), rect = R32(ctx.esp + 12);
    if (!LobbyMember() || !Valid(item, 0x18) || g_mission >= kMissionCount) return false;
    uint32_t id = R32(item + 0x14);
    if (id != 0x145 && id != 0x14B) return false;
    bool scoped = Valid(menu, 8);
    uint32_t descriptor = scoped ? R32(menu + 4) : 0;
    W32(item + 0x14, g_mission);  // Story's row draw: the mission is the row's id
    if (scoped) W32(menu + 4, kStoryHeader);
    kernel::CallGuest(ctx, 0x2CA9F0, {menu, item, rect});
    if (scoped) W32(menu + 4, descriptor);
    W32(item + 0x14, id);
    LobbyStatus(ctx, menu, rect);
    kernel::Return(ctx, 0, 0);
    return true;
}

// the lobby's settings as it sends them (0x354240, net_bots.cpp)
void StoryOnlineLobbyConfig() { LobbyConfig(); }

// most players a Story lobby takes (net_bots.cpp's capacity), 0 when not Story's
uint32_t StoryOnlinePlayerLimit() { return g_online && R32(kSessionUp) ? 4 : 0; }

// the match settings sent (0x356490) or loaded (0x658D0) at address: made Story's; true when they are
bool StoryOnlinePrepare(X86Context& ctx, uint32_t address) {
    uint32_t role = Role(), phase = R32(kLobbyState), caller = R32(ctx.esp);
    bool advance = role == 1 && (phase == 10 || phase == 14 || phase == 15) && (caller == 0x65CDC || caller == 0x69522);
    // a joined game's copy of the host's next load, as its GO arrives before the phase goes from 15 to 10
    bool received = role == 2 && phase == 15 && address == kMenuConfig && caller == 0x2D398D;
    if (!Session() || (role != 1 && role != 2) || (phase != 10 && phase != 11 && !advance && !received) ||
        !Valid(address, 0xA0))
        return false;
    uint8_t* config = At(address);
    if (role == 1 && phase == kHostLobby && address == kMenuConfig) {
        uint32_t humans = LastPlayer();
        if (!humans || humans > 4 || !config[1] || config[1] > humans || g_mission >= kMissionCount ||
            g_difficulty >= 3)
            return false;
        uint8_t header[0x14];
        std::memcpy(header, config, sizeof header);
        uint32_t mode = R32(kMenuMode), mission = R32(kMenuMission), difficulty = R32(kMenuDifficulty);
        W32(kMenuMode, kStoryMode), W32(kMenuMission, g_mission), W32(kMenuDifficulty, g_difficulty);
        W32(kMenuStage, 0);
        kernel::CallGuest(ctx, 0x65940, {address});  // Story's first stage: the mission's intro, or its level
        W32(kMenuMode, mode), W32(kMenuMission, mission), W32(kMenuDifficulty, difficulty);
        std::memcpy(config, header, sizeof header);
        config[0] = uint8_t(humans), config[2] = 0;
        config[0x57] = uint8_t(g_difficulty);
        for (uint32_t i = 0; i < humans; i++) config[0x14 + i] = 12, config[0x24 + i] = 0;
    }
    if (advance) {
        const uint8_t* active = At(kPlayConfig);
        if (!active[0] || active[0] > 4 || active[1] != 1) return false;
        // the intro's end builds a local load: the session's header kept
        std::memcpy(config, active, 0x14);
        config[2] = 0;
        config[0x57] = uint8_t(g_difficulty);
        for (uint32_t i = 0; i < config[0]; i++) config[0x14 + i] = 12, config[0x24 + i] = 0;
    }
    uint32_t flags;
    std::memcpy(&flags, config + 0x58, 4);
    bool rewards = config[0x54] == 106 && config[0x55] == 9 && config[0x56] == 3 && !flags;
    if (!config[0] || config[0] > 4 || !config[1] || config[1] > config[0] || config[2] ||
        (!rewards && (config[0x55] != 10 || config[0x56] != 2)) || config[0x57] >= 3 ||
        (!rewards && flags != kStoryFlags && flags != kMovieFlag))
        return false;
    uint32_t selected = rewards ? g_mission : kMissionCount, stage = rewards ? 3 : 1;
    for (uint32_t i = 0; i < kMissionCount && !rewards; i++) {
        uint32_t row = kMissions + i * kMissionSize;
        if (flags == kMovieFlag) {
            for (uint32_t offset = 0x20; offset <= 0x24; offset += 4) {
                uint32_t movie = R32(row + offset), movie_scene = 0x51FC30 + movie * 36;
                if (movie < 256 && movie == config[0x9B] && Valid(movie_scene, 1) && At(movie_scene)[0] == config[0x54]) {
                    selected = i, stage = offset == 0x20 ? 0 : 2;
                    break;
                }
            }
            if (selected < kMissionCount) break;
        } else if (SceneOf(i) == int16_t(config[0x54])) {
            selected = i;
            break;
        }
    }
    if (selected >= kMissionCount) return false;
    g_mission = selected, g_difficulty = config[0x57], g_stage = stage;
    if (advance) {
        uint32_t generation = (kernel::CallGuest(ctx, 0x352640, {}) + 1) & 0xFF;
        if (!generation) generation = 1;
        kernel::CallGuest(ctx, 0x352650, {generation});
        kernel::CallGuest(ctx, 0x356490, {address});
    }
    LOGR("storyonline: load role %u scene %u mission %u difficulty %u players %u local %u stage %u%s", role,
         config[0x54], selected, config[0x57], config[0], config[1], g_stage, advance ? " (next load sent)" : "");
    return true;
}

// the pending load taken (after 0x658D0): one full view per game, the menus' Story fields set
void StoryOnlineActivate() {
    if (!Session()) return;
    // the network code reads through players and actors a Story level has not made yet (0): the page there reads as
    // zeros, as Doffman's port does, instead of faulting
    if (!mem::IsCommitted(0) && mem::Commit(0, 0x10000)) LOGR("storyonline: guest page 0 reads as zeros");
    W32(kMenuSetup, 4);
    g_start_pending = true;
    W32(kMenuMission, g_mission);
    W32(kMenuStage, g_stage == 3 ? 2 : g_stage);
    At(kMenuDifficulty)[0] = uint8_t(g_difficulty);
}

// Active(slot) from the actor update (0x3568C0 from 0xD8E5D, net_bots.cpp): a Story script actor runs before its body
bool StoryOnlineActorActive(uint32_t caller, uint32_t index, uint32_t entity) {
    if (caller != 0xD8E5D || !Session() || R32(kLobbyState) != kInGame || !StoryScene()) return false;
    const uint8_t* config = At(kPlayConfig);
    uint32_t role = Role(), humans = config[0];
    if ((role != 1 && role != 2) || !humans || humans > 4 || config[0x55] != 10 || index < humans || index >= 32)
        return false;
    uint32_t pool = R32(kActors), actor = pool + index * kActorSize;
    if (!pool || !Valid(actor, kActorSize)) return false;
    uint32_t body = R32(actor);
    return entity && Valid(entity, 0x154) && R32(actor + 8) == index && (!body || body == entity) &&
           R32(entity + 0x7C) == actor && !R32(entity + 0x150);
}

// StartPad(player) (0xCF060, story_four.cpp, after the game's): the opening's starts before its first checkpoint
bool StoryOnlineSpawnPad(X86Context& ctx, uint32_t context) {
    if (!OpeningStarts() || !Valid(context, 0x1508) || !Prepare()) return false;
    uint32_t slot = R32(context), pool = R32(kPlayerArray);
    if (slot >= R32(kPlayers) || !pool || context != pool + slot * kPlayerSize) return false;
    if (kernel::CallGuest(ctx, 0x25EE60, {0})) return false;  // a checkpoint reached
    ctx.eax = slot == 0 ? kStart1 : slot == 1 ? kStart2 : kExtraStart + slot;
    static int logged = 0;
    if (logged++ < 16) LOGR("storyonline: player %u starts at pad %X", slot + 1, ctx.eax);
    return true;
}

}  // namespace game

// ---- the lobby's menus

// the lobby body's draw: its menu and rectangle for the details below
X86_EXTERN_FUNC(__imp__sub_00198E20);
X86_FUNC(sub_00198E20) {
    uint32_t menu = g_body_menu, rect = g_body_rect;
    g_body_menu = R32(ctx.esp + 4), g_body_rect = R32(ctx.esp + 12);
    __imp__sub_00198E20(ctx, base);
    g_body_menu = menu, g_body_rect = rect;
}

// the lobby's details: the mission's text
X86_EXTERN_FUNC(__imp__sub_001986A0);
X86_FUNC(sub_001986A0) {
    if (LobbyMember() && R32(ctx.esp) == 0x199650 && Valid(g_body_menu, 0xF8) && Valid(g_body_rect, 16) &&
        g_mission < kMissionCount && Prepare()) {
        float l = F(g_body_rect), t = F(g_body_rect + 4), r = F(g_body_rect + 8), b = F(g_body_rect + 12);
        if (r > l + 32.0f && b > t + 32.0f) {
            uint32_t font = kernel::CallGuest(ctx, kFont, {3});
            uint32_t text = kernel::CallGuest(ctx, kText, {R32(kMissions + 0x10 + g_mission * kMissionSize)});
            float x = l + 8.0f, y = t + (b - t) * 0.68f, width = r - l - 16.0f;
            uint32_t old_x = R32(kTextScaleX), old_y = R32(kTextScaleY);
            float sx = F(kTextScaleX), sy = F(kTextScaleY), scale = 1.0f;
            uint32_t wrapped = 0;
            for (int attempt = 0; attempt < 9; attempt++) {
                SetF(kTextScaleX, sx * scale), SetF(kTextScaleY, sy * scale);
                kernel::CallGuest(ctx, kApplyScale, {});
                wrapped = kernel::CallGuest(ctx, kWrap, {text, Bits(width)});
                kernel::CallGuest(ctx, kMeasure, {g_scratch, g_scratch + 4, wrapped, 0});
                float height = F(g_scratch + 4);
                if (height >= 0.0f && height <= b - y - 8.0f) break;
                scale *= 0.85f;
            }
            uint32_t colour = kernel::CallGuest(ctx, kFade, {0x7F7F7F7F, R32(g_body_menu + 0xF4)});
            kernel::CallGuest(ctx, kDrawText, {Bits(x), Bits(y), wrapped, colour});
            W32(kTextScaleX, old_x), W32(kTextScaleY, old_y);
            kernel::CallGuest(ctx, kApplyScale, {});
            kernel::CallGuest(ctx, kFont, {font});
            kernel::Return(ctx, 0, 0);
            return;
        }
    }
    __imp__sub_001986A0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001A8D30);
X86_FUNC(sub_001A8D30) { LobbyTick(ctx, base, __imp__sub_001A8D30, kLobbyHost); }

X86_EXTERN_FUNC(__imp__sub_001A8F30);
X86_FUNC(sub_001A8F30) {
    if (LobbyHost()) return __imp__sub_001A8F30(ctx, base);
    LobbyTick(ctx, base, __imp__sub_001A8F30, kLobbyJoined);
}

// Story's mission list, built for the host's lobby with Story's mode, mission and difficulty
X86_EXTERN_FUNC(__imp__sub_002CB090);
X86_FUNC(sub_002CB090) {
    if (!LobbyHost() || !g_options) return __imp__sub_002CB090(ctx, base);
    uint32_t mode = R32(kMenuMode), mission = R32(kMenuMission), difficulty = R32(kMenuDifficulty);
    W32(kMenuMode, kStoryMode), W32(kMenuMission, g_mission), W32(kMenuDifficulty, g_difficulty);
    __imp__sub_002CB090(ctx, base);
    W32(kMenuMode, mode), W32(kMenuMission, mission), W32(kMenuDifficulty, difficulty);
}

// a mission picked in the host's lobby: the difficulty menu, no local load
X86_EXTERN_FUNC(__imp__sub_002CAFE0);
X86_FUNC(sub_002CAFE0) {
    uint32_t menu = R32(ctx.esp + 4);
    if (!LobbyHost() || g_options != 1) return __imp__sub_002CAFE0(ctx, base);
    if (Valid(menu, kChosen + 4) && R32(menu + kChosen) < kMissionCount && Prepare()) {
        g_mission = R32(menu + kChosen);
        g_options = 2;
        kernel::CallGuest(ctx, kShowMenu, {g_parent, g_difficulties});
    }
    kernel::Return(ctx, 0, 0);
}

// ---- the load

// ReadyExchange(): the network's ready packet without Arcade's staging pads, which would move Story's starts
X86_EXTERN_FUNC(__imp__sub_00357540);
X86_FUNC(sub_00357540) {
    uint32_t flags = R32(kGameFlags), players = R32(kPlayers), role = Role();
    bool scoped = Session() && (role == 1 || role == 2) && Valid(R32(kLocalNode), 0x35) &&
                  ((flags & kMovieFlag) || g_stage == 3 || (flags == kStoryFlags && g_start_pending && g_stage == 1)) &&
                  players && players <= 4 && (R32(kScene) < 100 || (R32(kScene) == 106 && g_stage == 3));
    if (!scoped) return __imp__sub_00357540(ctx, base);
    W32(kGameFlags, flags & ~kMovieFlag);
    W32(kPlayers, 0);
    __imp__sub_00357540(ctx, base);
    W32(kPlayers, players), W32(kGameFlags, flags);
}

// NextLoad(): the host's alone after the intro; a joined game waits for the host's
X86_EXTERN_FUNC(__imp__sub_00065C70);
X86_FUNC(sub_00065C70) {
    uint32_t phase = R32(kLobbyState);
    if (Session() && (phase == 10 || phase == 14 || phase == 15) && Role() == 2 &&
        ((R32(kGameFlags) & kMovieFlag) || g_stage >= 2)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_00065C70(ctx, base);
}

// SceneRecords(flags): a Story scene's records as co-op's; those for two for up to four
X86_EXTERN_FUNC(__imp__sub_0006B100);
X86_FUNC(sub_0006B100) {
    // the original pops its return address: the argument's slot kept from before
    uint32_t arg = ctx.esp + 4, flags = R32(arg), players = R32(kPlayers);
    bool adapt = Session() && StoryScene() && players && players <= 4;
    if (adapt) {
        uint32_t authored = flags & ~0x80000000u;
        if (players > 2 && (authored & 0x00400000)) authored &= ~0x00600000u;
        W32(arg, authored);
    }
    __imp__sub_0006B100(ctx, base);
    if (adapt) W32(arg, flags);
}

// StartRecord(pad): the two made starts of the opening, beside its first
X86_EXTERN_FUNC(__imp__sub_0009A420);
X86_FUNC(sub_0009A420) {
    uint32_t pad = R32(ctx.esp + 4);
    if (!OpeningStarts() || (pad != kExtraStart + 2 && pad != kExtraStart + 3) || !Prepare())
        return __imp__sub_0009A420(ctx, base);
    uint32_t source = kernel::CallGuest(ctx, 0x9A420, {kStart1});
    uint32_t target = g_extra_pads + (pad - kExtraStart - 2) * 32;
    if (!Valid(source, 32)) return kernel::Return(ctx, source, 0);
    std::memcpy(At(target), At(source), 32);
    SetF(target + 12, F(target + 12) + (pad == kExtraStart + 2 ? -2.0f : 2.0f));
    kernel::Return(ctx, target, 0);
}

// StartTag(pad): a made start's as the first's
X86_EXTERN_FUNC(__imp__sub_0009A4C0);
X86_FUNC(sub_0009A4C0) {
    uint32_t pad = R32(ctx.esp + 4);
    if (!OpeningStarts() || (pad != kExtraStart + 2 && pad != kExtraStart + 3)) return __imp__sub_0009A4C0(ctx, base);
    uint32_t arg = ctx.esp + 4;
    W32(arg, kStart1);
    __imp__sub_0009A4C0(ctx, base);
    W32(arg, pad);
}

// StartIndex(record): a made start's pad
X86_EXTERN_FUNC(__imp__sub_0009A510);
X86_FUNC(sub_0009A510) {
    uint32_t record = R32(ctx.esp + 4);
    if (OpeningStarts() && g_extra_pads && (record == g_extra_pads || record == g_extra_pads + 32))
        return kernel::Return(ctx, kExtraStart + 2 + (record - g_extra_pads) / 32, 0);
    __imp__sub_0009A510(ctx, base);
}

// LevelFrame(): the players' contexts started at Story's own starts once, the humans on team 1
X86_EXTERN_FUNC(__imp__sub_000681C0);
X86_FUNC(sub_000681C0) {
    if (g_start_pending && g_stage == 1 && Session() && R32(kLobbyState) == kInGame && R32(kEndType) == 7 &&
        !R32(kResult)) {
        uint32_t count = R32(kPlayers), pool = R32(kPlayerArray);
        const uint8_t* config = At(kPlayConfig);
        if (count && count <= 4 && count == config[0] && config[1] == 1 && config[0x55] == 10 && config[0x56] == 2 &&
            pool && Valid(pool, (count - 1) * kPlayerSize + 0x1508)) {
            g_start_pending = false;
            uint32_t started = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t context = pool + i * kPlayerSize, state = R32(context + 0x114);
                if (state != 16 && state != 4) continue;
                W32(context + 0x1504, 0x7FFFFFFF);  // not the map pad the network's staging cached
                kernel::CallGuest(ctx, 0xD50E0, {context, 1});
                started++;
            }
            LOGR("storyonline: %u of %u players started at Story's starts", started, count);
        }
    }
    if (Session()) HumanTeams(ctx);
    __imp__sub_000681C0(ctx, base);
}

// InitTeam(actor): a human on team 1
X86_EXTERN_FUNC(__imp__sub_000D7570);
X86_FUNC(sub_000D7570) {
    uint32_t actor = R32(ctx.esp + 4);
    __imp__sub_000D7570(ctx, base);
    if (HumanTeamScope(actor)) HumanTeams(ctx);
}

// SetTeam(actor, team) from the network (0x33ACDB): a human on team 1
X86_EXTERN_FUNC(__imp__sub_000D7550);
X86_FUNC(sub_000D7550) {
    uint32_t actor = R32(ctx.esp + 4), arg = ctx.esp + 8, team = R32(arg);
    bool scope = R32(ctx.esp) == 0x33ACDB && HumanTeamScope(actor);
    if (scope) W32(arg, 1);
    __imp__sub_000D7550(ctx, base);
    if (scope) W32(arg, team);
}

// RemoveCharacter(body) from the network tick (0x355EA4): Story's characters stay on the list
X86_EXTERN_FUNC(__imp__sub_0015BCC0);
X86_FUNC(sub_0015BCC0) {
    if (Session() && (g_start_pending || (R32(kScene) < 100 && At(kGameType)[0] == 10)) && R32(ctx.esp) == 0x355EA4) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_0015BCC0(ctx, base);
}

// the players' payloads: not while a player has no body (movies, loads)
void Payload(X86Context& ctx, uint8_t* base, void (*original)(X86Context&, uint8_t*)) {
    if (Session()) {
        uint32_t count = R32(kPlayers), contexts = R32(kPlayerArray);
        bool ready = count && count <= 16 && R32(kScene) < 100 && contexts && Valid(contexts, count * kPlayerSize) &&
                     !(R32(kGameFlags) & kMovieFlag);
        for (uint32_t i = 0; ready && i < count; i++) {
            uint32_t context = contexts + i * kPlayerSize, body = R32(context + 0x14);
            ready = R32(context) == i && Valid(body, 0x25C) && R32(body + 0x150) == context &&
                    Valid(R32(body + 0x7C), kActorSize);
        }
        if (!ready) return kernel::Return(ctx, 0, 0);
    }
    original(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0035BD70);
X86_FUNC(sub_0035BD70) { Payload(ctx, base, __imp__sub_0035BD70); }
X86_EXTERN_FUNC(__imp__sub_0035DB60);
X86_FUNC(sub_0035DB60) { Payload(ctx, base, __imp__sub_0035DB60); }

// NetworkSlot(local): the network player of this game's local player, through its record (local node [0x774028]:
// +0x20 the records' count, +0xC the records, +0x3C a record's player). A movie of a Story mission reads it for the
// profile (0x69640) before the records are made: the local index then, as with no session.
X86_EXTERN_FUNC(__imp__sub_00356800);
X86_FUNC(sub_00356800) {
    uint32_t local = R32(ctx.esp + 4), node = R32(kLocalNode);
    if (R32(kScene) != 0x65 && R32(kSessionUp) && Valid(node, 0x24) && local < 4 &&
        int32_t(R32(node + 0x20)) <= int32_t(R32(kPlayers)) && !Valid(R32(node + 0xC + local * 4), 0x40)) {
        static int logged = 0;
        if (logged++ < 8) LOGR("storyonline: local player %u has no network record yet, its own profile", local + 1);
        return kernel::Return(ctx, local, 0);
    }
    __imp__sub_00356800(ctx, base);
}

// SendPlayerBlock(record): sends the 0x7C bytes its player's +0x1500 points at when they change; a Story player has
// none yet (0), which the Xbox read as zeros and the PC faults on: nothing to send then
X86_EXTERN_FUNC(__imp__sub_00358EB0);
X86_FUNC(sub_00358EB0) {
    uint32_t record = R32(ctx.esp + 4);
    if (R32(kSessionUp) && Valid(record, 0x40) && Valid(R32(kPlayerArray), 4)) {
        uint32_t player = R32(kPlayerArray) + R32(record + 0x3C) * kPlayerSize;
        if (!Valid(player + 0x1500, 4) || !Valid(R32(player + 0x1500), 0x7C)) {
            static int logged = 0;
            if (logged++ < 4) LOGR("storyonline: player %u has no block to send yet", R32(record + 0x3C));
            return kernel::Return(ctx, 0, 0);
        }
    }
    __imp__sub_00358EB0(ctx, base);
}

// ShowMenu(parent, descriptor): after a mission's rewards Story goes on to its own mission list, which has nowhere
// to go back to in a session: the lobby instead, where the host picks the next mission
X86_EXTERN_FUNC(__imp__sub_00079B70);
X86_FUNC(sub_00079B70) {
    uint32_t arg = ctx.esp + 8, descriptor = R32(arg), role = Role();
    if (descriptor != kStoryList || g_options || !g_online || !R32(kSessionUp) || g_stage < 2 || (role != 1 && role != 2))
        return __imp__sub_00079B70(ctx, base);
    uint32_t lobby = role == 1 ? kLobbyHost : kLobbyJoined;
    LOGR("storyonline: mission %u over, back to the lobby", g_mission);
    g_stage = 0;
    W32(arg, lobby);
    __imp__sub_00079B70(ctx, base);
    W32(arg, descriptor);
}

// SingleView(cue): two-player Story's cutscene policy shows one player's view, clearing the other camera's world
// render (+0x21C bit 0); online each game has its own screen, and the joined one's player is the one cleared: its
// screen stayed black. A local player's camera keeps it (as Doffman's port does)
X86_EXTERN_FUNC(__imp__sub_000CB110);
X86_FUNC(sub_000CB110) {
    __imp__sub_000CB110(ctx, base);
    if (!Session() || R32(kScene) >= 100 || At(kGameType)[0] != 10) return;
    uint32_t pool = R32(kPlayerArray), players = R32(kPlayers);
    for (uint32_t slot = 0; slot < players && slot < 4 && pool; slot++) {
        uint32_t context = pool + slot * kPlayerSize;
        if (!Valid(context, 0x10) || R32(context + 4) != 0) continue;  // +4: 0 for a player on this PC
        uint32_t camera = R32(context + 8);
        if (!Valid(camera, 0x220) || (R32(camera + 0x21C) & 1)) continue;
        W32(camera + 0x21C, R32(camera + 0x21C) | 1);
        static int logged = 0;
        if (logged++ < 4) LOGR("storyonline: player %u's view kept on (the single view policy)", slot + 1);
    }
}

// SendActorStates(): every player's actor (the pool at 0x7356D8) packed for the others; in Story the network runs
// before the level has made its actors, the pool still 0: nothing to send until it has
X86_EXTERN_FUNC(__imp__sub_0035A480);
X86_FUNC(sub_0035A480) {
    if (R32(kSessionUp) && !Valid(R32(kActors), kActorSize)) {
        static int logged = 0;
        if (logged++ < 4) LOGR("storyonline: no actors yet, their states not sent");
        return kernel::Return(ctx, 0, 0);
    }
    __imp__sub_0035A480(ctx, base);
}
