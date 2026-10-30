// The free camera: F6 (or Back with the right stick clicked) leaves the body where it stands and flies the view through
// the level, the player unhurt while it is out. Not in Arcade, a network game or an online co-op session: Story only.
//
// The view: the player's camera ([0x7B0CC0]) is updated by sub_000CCAB0, which places it (+0x27C), points it (+0x334,
// with +0x328 the point looked at, +0x358 a copy of the direction and +0x370 up x direction) and finds the room it is
// in (+0x2A0, sub_000C3C50(point, 1.0, last room)), the portals drawn from there. It ends with the view matrix,
// LookAt(matrix, place, direction, up) (sub_001519E0, from 0xCEB98), gluLookAt's: the screen's right is direction x up.
// While the camera is free that call takes its place and direction instead, and the camera's fields and room follow.
//
// The controls (port 1's, kept from the game while the camera is free): the left stick or WASD moves along the view,
// the right stick, arrows or the mouse turn it; A (Space) rises, the left stick's click (Ctrl, C) sinks; the right
// trigger or Shift fly faster, the left trigger slower.
//
// The whole level drawn: what is drawn is chosen each frame from the camera's room (sub_00089B90, at the end of
// sub_001CE650 and sub_001CE850): every room's entry in the camera (+0x3BC + room * 12: the depth it was reached at, -1
// none, then its rectangle on screen as min x, min y, max x, max y in shorts) is cleared by sub_000878A0, then the
// portals are followed from the camera's room (sub_00087F80(room, depth, min x, max x, min y, max y)) and each room
// reached opens up its rectangle. While the camera is free every room of the level ([0x7B975C] of them) is opened to
// the whole view ([0x7E71C8], as sub_00089B90 passes it), so the level is all there from anywhere, outside it as well.
//
// Neither the HUD (its root, sub_0014EF40(player)) nor the held weapon and the arms holding it (game/fov.cpp) are
// drawn while the camera is free.
//
// Unhurt: damage goes through sub_000BABA0(record), the record's +0 the body hit and +0x20 the damage (TheDoffman/
// timesplitters-future-perfect-pc, docs/online-bots-investigation.md); the free player's is dropped. The body's health
// (+0x1CC, of the most +0x1D8) is also kept full each frame against what does not pass there.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>


#include "common/mouse.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kCamera = 0x7B0CC0;
constexpr uint32_t kGameMode = 0x7DE454;  // its second byte 10 in Story
constexpr uint32_t kPlayerArray = 0x7B0C48, kPlayerSize = 0x1584, kPlayers = 0x790950;
constexpr uint32_t kBody = 0x14, kHealth = 0x1CC, kMostHealth = 0x1D8;
constexpr uint32_t kFromView = 0xCEB9D;  // the player camera's LookAt
constexpr uint32_t kRoomOf = 0xC3C50;
constexpr uint32_t kRooms = 0x708788, kRoomSize = 0xB4;
// the pad's bits and analog buttons (core/input.h)
constexpr uint16_t kLeftThumb = 1 << 6;
constexpr int kA = 0, kLeftTrigger = 6, kRightTrigger = 7;

std::atomic<bool> g_wanted{false};  // asked for by the key
std::atomic<bool> g_on{false};      // flying
std::mutex g_pad_lock;
input::Pad g_pad;  // port 1's controls while flying

struct Fly {
    uint32_t camera = 0;
    float p[3] = {}, yaw = 0, pitch = 0;
    std::chrono::steady_clock::time_point at;
} g_fly;

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}
float& F(uint32_t va) { return kernel::G<float>(va); }
bool Story() { return ((kernel::R32(kGameMode) >> 8) & 0xFF) == 10; }
bool NetworkGame() { return game::FovLocked() || lockstep::Active(); }
uint32_t FirstPlayer() {
    uint32_t first = kernel::R32(kPlayerArray);
    return kernel::R32(kPlayers) && Valid(first, kPlayerSize) ? first : 0;
}
uint32_t FirstBody() {
    uint32_t player = FirstPlayer(), body = player ? kernel::R32(player + kBody) : 0;
    return Valid(body, kMostHealth + 4) ? body : 0;
}


float Axis(int16_t v) {
    float x = float(v) / 32767.0f;
    return std::abs(x) < 0.2f ? 0.0f : x;  // past stick drift
}

// a frame of flight: the place and angles moved by the controls since the last
void Steer(float dt) {
    input::Pad pad;
    {
        std::lock_guard lock(g_pad_lock);
        pad = g_pad;
    }
    float mx = 0, my = 0;
    mouse::Aiming();
    mouse::TakeMotion(mx, my);
    const game::Options& o = game::Opt();
    float turn = 0.0025f * float(o.mouse_sensitivity) / 100.0f;
    // the screen's right is direction x up: turning right lowers the yaw
    g_fly.yaw -= mx * turn + Axis(pad.rx) * 2.5f * dt;
    float up_down = my * turn * float(o.mouse_vertical) / 100.0f * (o.invert_mouse ? -1.0f : 1.0f);
    g_fly.pitch = std::clamp(g_fly.pitch - up_down + Axis(pad.ry) * 2.0f * dt, -1.55f, 1.55f);

    float cy = std::cos(g_fly.yaw), sy = std::sin(g_fly.yaw), cp = std::cos(g_fly.pitch), sp = std::sin(g_fly.pitch);
    float d[3] = {sy * cp, sp, cy * cp}, right[3] = {-cy, 0, sy};
    float speed = 8.0f;
    if (pad.analog[kRightTrigger] > 0x40 || GetAsyncKeyState(VK_SHIFT) < 0) speed *= 4;
    if (pad.analog[kLeftTrigger] > 0x40) speed *= 0.25f;
    float forward = Axis(pad.ly), side = Axis(pad.lx);
    float rise = (pad.analog[kA] > 0x40 ? 1.0f : 0.0f) - ((pad.buttons & kLeftThumb) ? 1.0f : 0.0f);
    for (int i = 0; i < 3; i++) g_fly.p[i] += (d[i] * forward + right[i] * side) * speed * dt;
    g_fly.p[1] += rise * speed * dt;
}

void KeepUnhurt() {
    uint32_t body = FirstBody();
    if (body && F(body + kHealth) < F(body + kMostHealth)) F(body + kHealth) = F(body + kMostHealth);
}

}  // namespace

namespace game {
bool CheatUnhurt(uint32_t body);  // game/cheats.cpp: Invincible
void CheatKeepUnhurt();

// pad poll: F6 or Back with the right stick clicked
void ToggleFreeCam() { g_wanted = !g_wanted; }

// pad poll, port 1: while flying the controls are the camera's, true and the game sees an idle pad
bool FreeCamTakes(const input::Pad& pad) {
    if (!g_on) return false;
    std::lock_guard lock(g_pad_lock);
    g_pad = pad;
    return true;
}

bool FreeCamOn() { return g_on; }

}  // namespace game

// LookAt(matrix, place, direction, up): the player camera's view from the free camera's place and direction
X86_EXTERN_FUNC(__imp__sub_001519E0);
X86_FUNC(sub_001519E0) {
    uint32_t caller = kernel::R32(ctx.esp), camera = kernel::R32(kCamera), player = FirstPlayer();
    bool ours = caller == kFromView && player && Valid(camera, 0x380) && kernel::R32(player + 8) == camera;
    if (ours) game::CheatKeepUnhurt();
    if (ours) {
        bool allowed = Story() && !NetworkGame();
        bool want = g_wanted && allowed;
        if (g_wanted && !allowed) {
            g_wanted = false;
            LOGR("freecam: not here (mode %u, network %d)", (kernel::R32(kGameMode) >> 8) & 0xFF, int(NetworkGame()));
        }
        auto now = std::chrono::steady_clock::now();
        if (want && (!g_on || g_fly.camera != camera)) {
            // taken from where the game's view is
            uint32_t args = ctx.esp + 8;
            for (int i = 0; i < 3; i++) g_fly.p[i] = F(args + i * 4);
            float d[3] = {F(args + 12), F(args + 16), F(args + 20)};
            float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-4f) {
                g_fly.yaw = std::atan2(d[0], d[2]);
                g_fly.pitch = std::asin(std::clamp(d[1] / len, -1.0f, 1.0f));
            }
            g_fly.camera = camera;
            g_fly.at = now;
            {
                std::lock_guard lock(g_pad_lock);
                g_pad = {};
            }
            LOGR("freecam: on at %.2f %.2f %.2f", g_fly.p[0], g_fly.p[1], g_fly.p[2]);
        } else if (!want && g_on) {
            LOGR("freecam: off");
        }
        g_on = want;
        if (g_on) {
            float dt = std::clamp(std::chrono::duration<float>(now - g_fly.at).count(), 0.0f, 0.1f);
            g_fly.at = now;
            Steer(dt);
            KeepUnhurt();
            float cy = std::cos(g_fly.yaw), sy = std::sin(g_fly.yaw), cp = std::cos(g_fly.pitch), sp = std::sin(g_fly.pitch);
            float d[3] = {sy * cp, sp, cy * cp};
            uint32_t args = ctx.esp + 8;
            // straight up, without the sway the game's camera breathes with (its up, +0x364, rolls)
            float u[3] = {0.0f, 1.0f, 0.0f};
            for (int i = 0; i < 3; i++) {
                F(args + 24 + i * 4) = u[i];
                F(camera + 0x364 + i * 4) = u[i];
                F(args + i * 4) = g_fly.p[i];
                F(args + 12 + i * 4) = d[i];
                F(camera + 0x27C + i * 4) = g_fly.p[i];
                F(camera + 0x334 + i * 4) = d[i];
                F(camera + 0x358 + i * 4) = d[i];
                F(camera + 0x328 + i * 4) = g_fly.p[i] + d[i];
            }
            // up x direction, as the game keeps it
            float s[3] = {u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0]};
            float n = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            if (n > 1e-4f)
                for (int i = 0; i < 3; i++) F(camera + 0x370 + i * 4) = s[i] / n;
            // the room the free camera is in, for the portals drawn; outside them all, the last kept
            uint32_t room =
                kernel::CallGuest(ctx, kRoomOf, {camera + 0x27C, std::bit_cast<uint32_t>(1.0f), kernel::R32(camera + 0x2A0)});
            // a room the table has (0x708788, 0xB4 each, +4 its portal list as 0x87720 reads it); anything else, as
            // the lookup gives for a point outside them all, would be read past the table: the last kept
            uint32_t table = kernel::R32(kRooms);
            bool real = int32_t(room) > 0 && room < 0x1000 && Valid(table + room * kRoomSize, kRoomSize) &&
                        Valid(kernel::R32(table + room * kRoomSize + 4), 4);
            static int logged = 0;
            if (logged < 20 && room != kernel::R32(camera + 0x2A0)) {
                logged++;
                LOGR("freecam: room %d%s (was %d)", int32_t(room), real ? "" : ", not one: kept",
                     int32_t(kernel::R32(camera + 0x2A0)));
            }
            if (real) kernel::W32(camera + 0x2A0, room);
        }
    }
    __imp__sub_001519E0(ctx, base);
}

namespace {
constexpr uint32_t kRoomCount = 0x7B975C, kView = 0x7E71C8;
constexpr uint32_t kRoomSeen = 0x3BC, kRoomSeenSize = 12, kDeepest = 0xD28;

// every room of the level open to the whole view, from the free camera
void ShowAllRooms() {
    uint32_t camera = kernel::R32(kCamera), player = FirstPlayer(), view = kernel::R32(kView);
    if (!g_on || !player || kernel::R32(player + 8) != camera || !Valid(camera, kDeepest + 4) || !Valid(view, 0x1C)) return;
    int32_t left = int32_t(kernel::R32(view)), right = int32_t(kernel::R32(view + 4));
    int32_t top = int32_t(kernel::R32(view + 0x14)), bottom = int32_t(kernel::R32(view + 0x18));
    // the entries end where the deepest is kept
    uint32_t rooms = std::min(kernel::R32(kRoomCount), (kDeepest - kRoomSeen) / kRoomSeenSize - 1);
    for (uint32_t room = 1; room <= rooms; room++) {
        uint32_t e = camera + kRoomSeen + room * kRoomSeenSize;
        if (int32_t(kernel::R32(e)) < 0) kernel::W32(e, 0);
        auto& minx = kernel::G<int16_t>(e + 4);
        auto& miny = kernel::G<int16_t>(e + 6);
        auto& maxx = kernel::G<int16_t>(e + 8);
        auto& maxy = kernel::G<int16_t>(e + 10);
        minx = int16_t(std::min<int32_t>(minx, left));
        miny = int16_t(std::min<int32_t>(miny, top));
        maxx = int16_t(std::max<int32_t>(maxx, right));
        maxy = int16_t(std::max<int32_t>(maxy, bottom));
    }
    if (int32_t(kernel::R32(camera + kDeepest)) < 0) kernel::W32(camera + kDeepest, 0);
}
}  // namespace

// the camera's update and the rooms it sees, for the two kinds of view: all the rooms while the camera is free
X86_EXTERN_FUNC(__imp__sub_001CE650);
X86_FUNC(sub_001CE650) {
    __imp__sub_001CE650(ctx, base);
    ShowAllRooms();
}

X86_EXTERN_FUNC(__imp__sub_001CE850);
X86_FUNC(sub_001CE850) {
    __imp__sub_001CE850(ctx, base);
    ShowAllRooms();
}

// the HUD of a player: none for the free one
X86_EXTERN_FUNC(__imp__sub_0014EF40);
X86_FUNC(sub_0014EF40) {
    if (g_on && kernel::R32(ctx.esp + 4) == 0) return kernel::Return(ctx, 0, 0);
    __imp__sub_0014EF40(ctx, base);
}

// Damage(record): none to the free player's body
X86_EXTERN_FUNC(__imp__sub_000BABA0);
X86_FUNC(sub_000BABA0) {
    uint32_t hit = kernel::R32(ctx.esp + 4);
    if (Valid(hit, 4) && game::CheatUnhurt(kernel::R32(hit))) return kernel::Return(ctx, 0, 0);
    if (g_on) {
        uint32_t record = kernel::R32(ctx.esp + 4), body = FirstBody();
        if (body && Valid(record, 4) && kernel::R32(record) == body) return kernel::Return(ctx, 0, 0);
    }
    __imp__sub_000BABA0(ctx, base);
}
