// True 1:1 mouse aim: the mouse turns the view itself, beside the stick's turn with its dead zone, ramp and speed.
// The current player's view angles are degrees, yaw at +0x100 (lower is right) and pitch at +0x104 (higher is up).
// The counts are applied as the frame's movement is built from the yaw (0xD2B80), after the stick's look step, so a
// step and a turn in the same frame agree.
//
// In an online co-op session (net/lockstep.h) each player turns by their own machine's mouse, the turn sent with their
// controls, sensitivity applied: both machines turn both players alike, and what decides a turn here is the game and
// the session, never this machine's mouse or settings.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>

#include "common/controls.h"
#include "common/mouse.h"
#include "common/options.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "net/lockstep.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kPlayer = 0x7B0C7C;      // the player being updated
constexpr uint32_t kCamera = 0x7B0CC0;      // its camera
constexpr uint32_t kPitchLimit = 0x7B0C80;  // degrees up or down
constexpr uint32_t kPauseState = 0x761C48;  // the pause screen's state: +0 bit 0 up
constexpr uint32_t kSlot = 0x4;             // the local input slot; +0 is a player id, not the pad
constexpr uint32_t kYaw = 0x100, kPitch = 0x104;
constexpr uint32_t kGoliathState = 0x100;  // the Goliath walker's seat
constexpr uint32_t kState = 0x114;          // 0x80 a mounted gun, 0x40 0x20 0x100 0x200 0x400 views of their own
constexpr uint32_t kFlags = 0xC;            // bit 7: the view turns whatever the camera
constexpr uint32_t kControls = 0x1500;      // the player's control settings: +2 bit 3 auto lookahead
constexpr uint32_t kCamView = 0x26C;        // the camera's view: type 0x18 is the player's own
constexpr uint32_t kCamFov = 0x384;         // degrees, narrower when zoomed
constexpr float kBaseFov = 55.0f;           // the unzoomed view
constexpr float kDegreesPerCount = 0.10f;   // at sensitivity 100
constexpr float kPi = 3.14159265f;

float F(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }
void SetF(uint32_t va, float v) { kernel::W32(va, std::bit_cast<uint32_t>(v)); }

bool Session() { return lockstep::Active(); }
// a player the mouse turns: the first input slot's, in a session either player's
bool MouseSlot(uint32_t slot) { return Session() ? slot < uint32_t(lockstep::kPlayers) : slot == 0; }

// the keyboard and mouse player: the current player on the first input slot (in a session, either)
uint32_t LocalPlayer() {
    uint32_t player = kernel::R32(kPlayer);
    return player && MouseSlot(kernel::R32(player + kSlot)) ? player : 0;
}
// this machine's own player, for what only shows here (the cursor, the wheel's zoom)
bool Own(uint32_t player) {
    return player && kernel::R32(player + kSlot) == uint32_t(Session() ? lockstep::LocalPort() : 0);
}
// the player plays on the keyboard and mouse
bool PlayerKeyboard(uint32_t player) {
    return Session() ? player && lockstep::PortKeyboard(int(kernel::R32(player + kSlot))) : controls::KeyboardActive();
}
// the player's mouse counts this frame: this machine's, or in a session the frame's from the network
bool TakeCounts(uint32_t player, float& dx, float& dy) {
    if (!Session()) return !game::FreeCamOn() && mouse::TakeMotion(dx, dy);  // the free camera's while it flies
    return player && lockstep::PortAim(int(kernel::R32(player + kSlot)), dx, dy) && (dx != 0 || dy != 0);
}

// the view's own turning: the states the look update turns the view in
bool ViewTurns(uint32_t player) {
    uint32_t state = kernel::R32(player + kState);
    if (state == 0x80 || state == 0x40 || state == 0x20 || state == 0x100 || state == 0x2000 || state == 0x10000)
        return false;
    if (kernel::R32(player + kFlags) & 0x80) return true;
    uint32_t cam = kernel::R32(kCamera);
    uint32_t view = cam ? kernel::R32(cam + kCamView) : 0;
    return view && kernel::R32(view) == 0x18 && state != 0x400 && state != 0x200;
}

// the camera's angle unzoomed: 55 degrees on a full screen, narrower in a split screen's view, so the widest a player's
// own view has had with that camera and player count, 55 until one is seen
std::map<uint64_t, float> g_unzoomed;
constexpr uint32_t kPlayers = 0x790950;
uint64_t ViewKey(uint32_t cam) { return uint64_t(cam) << 8 | (kernel::R32(kPlayers) & 0xFF); }
float UnzoomedFov(uint32_t cam) {
    auto it = g_unzoomed.find(ViewKey(cam));
    return it == g_unzoomed.end() ? kBaseFov : it->second;
}
// the player's own view: its angle is the widest yet, or narrower when zoomed
void SeeFov(uint32_t cam) {
    float fov = F(cam + kCamFov);
    if (!(fov >= 1.0f && fov <= 170.0f)) return;
    auto [it, added] = g_unzoomed.try_emplace(ViewKey(cam), fov);
    if (!added && fov > it->second) it->second = fov;
}

// zoomed in, a count turns the view less, so a movement covers the same part of the screen
float ZoomScale() {
    uint32_t cam = kernel::R32(kCamera);
    float base = cam ? UnzoomedFov(cam) : kBaseFov;
    float fov = cam ? F(cam + kCamFov) : base;
    if (!(fov >= 1.0f && fov < base)) return 1.0f;
    return std::tan(fov * kPi / 360.0f) / std::tan(base * kPi / 360.0f);
}

bool Valid(uint32_t va, uint32_t size) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

// a pause, objectives or status screen is up (the menus' own test, menu_mouse.cpp)
bool Paused() {
    uint32_t pause = kernel::R32(kPauseState);
    return pause >= 0x10000 && mem::IsCommitted(pause) && (kernel::R32(pause) & 1);
}

// a session's counts come with the sending machine's sensitivity, up and down share and inversion applied
float Step() { return kDegreesPerCount * (Session() ? 1.0f : float(game::Opt().mouse_sensitivity) / 100.0f); }
// the vertical counts as they turn things up: mouse up is up, unless inverted
float Vertical(float dy) {
    if (Session()) return dy;
    const game::Options& o = game::Opt();
    return dy * float(o.mouse_vertical) / 100.0f * (o.invert_mouse ? -1.0f : 1.0f);
}

// the mouse's counts for something of the local player's that turns now; none while paused
bool TakeTurn(uint32_t player, float& dx, float& dy) {
    if (Paused()) {
        TakeCounts(player, dx, dy);  // the pause screen's, not the view's
        return false;
    }
    if (Own(player)) mouse::Aiming();
    return player && player == LocalPlayer() && TakeCounts(player, dx, dy);
}

// A mounted thing's angles (a gun, a two-part camera): +0x1C up and down, lower is up, +0x24 around, lower is right,
// in degrees. The game clamps them to the thing's limits after its stick step, so the mouse adds first.
void TurnMounted(uint32_t angles, float scale) {
    float dx, dy;
    if (!Valid(angles, 0x48) || !TakeTurn(kernel::R32(kPlayer), dx, dy)) return;
    float step = Step() * scale;
    float pitch = F(angles + 0x1C), yaw = F(angles + 0x24);
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) return;
    SetF(angles + 0x1C, pitch + Vertical(dy) * step);
    SetF(angles + 0x24, yaw - dx * step);
}

// A terminal (U-Genix's): its pointer at +0x38 and +0x3C in 0-255 units, moved by the counts as the stick moves it, and
// the left button is its A. After the TimeSplitters Future Perfect PC port.
constexpr uint32_t kTerminalState = 0x2000, kTerminalSlots = 0x76C27C, kTicks = 0x4E7944;
void TerminalMouse(uint32_t player) {
    float dx, dy;
    if (Own(player)) mouse::InTerminal();
    if (!TakeTurn(player, dx, dy)) return;
    uint32_t slot = kernel::R32(player + kSlot);
    if (slot >= 4) return;
    uint32_t entity = kernel::R32(kTerminalSlots + slot * 4);
    uint32_t t = Valid(entity, 0x80) ? kernel::R32(entity + 0x7C) : 0;
    if (!Valid(t, 0x60) || kernel::R32(t + 0x14) != player || kernel::R32(t + 0x58)) return;
    uint32_t descriptor = kernel::R32(t + 0x30);
    if (!Valid(descriptor, 0x18) || (kernel::R32(descriptor + 0x14) & 4) || !kernel::R32(kTicks)) return;
    float x = F(t + 0x38), y = F(t + 0x3C);
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    float step = Step();
    SetF(t + 0x38, std::clamp(x + dx * step, 4.0f, 251.0f));
    SetF(t + 0x3C, std::clamp(y + dy * step, 4.0f, 251.0f));
    kernel::W32(t + 0x20, 0);  // the idle timer the stick resets
}

// the states a hook here turns 1:1; the others with a view of their own (a tank, a railbot, a big gun) take the mouse
// as the right stick (mouse.cpp VehicleStick)
bool TurnedHere(uint32_t state) { return state == 0 || state == 0x80 || state == 0x40 || state == 0x2000 || state == 0x10000; }
// A vehicle or gun's own step either adds the stick to the player's angles each frame (a tank's turret, a railbot) or
// sets them from the stick's tilt. Which one is learned the first time the mouse moves in it: the counts are added
// before the step, and kept by it they turn it 1:1 from then on; lost, the mouse is the right stick there.
enum class VehicleTurn : int8_t { Unknown, Direct, Stick };
std::map<uint64_t, VehicleTurn> g_vehicle_turn;
uint64_t VehicleKey(uint32_t player) {
    uint32_t body = kernel::R32(player + 0x14);
    uint32_t parts = Valid(body, 0x80) ? kernel::R32(body + 0x7C) : 0;
    uint32_t handle = Valid(parts, 0xC0C) ? kernel::R32(parts + 0xC08) : 0;  // the vehicle ridden
    return uint64_t(kernel::R32(player + kState)) << 32 | handle;
}

void MarkVehicle(uint32_t player) {
    if (player && g_vehicle_turn[VehicleKey(player)] == VehicleTurn::Direct) return;
    // only where the view does not turn itself: on foot some states (crouching, jumping) are not 0 either
    if (player && Own(player) && !TurnedHere(kernel::R32(player + kState)) && !ViewTurns(player) && !Paused()) {
        mouse::InVehicle();
        mouse::Aiming();
    }
}

void ApplyMouse() {
    uint32_t player = LocalPlayer();
    if (!player) return;
    if (kernel::R32(player + kState) == kTerminalState) {
        TerminalMouse(player);
        return;
    }
    if (Paused()) {
        // the view keeps updating under the pause screen: the mouse is the menu's, the cursor shows
        float dx, dy;
        TakeCounts(player, dx, dy);
        return;
    }
    // the mounted views take their counts in their own updates
    if (!ViewTurns(player)) {
        MarkVehicle(player);
        return;
    }
    bool own = Own(player);
    if (own) mouse::Aiming();
    uint32_t cam = kernel::R32(kCamera);
    if (cam) SeeFov(cam);
    bool zoomed = cam && F(cam + kCamFov) < UnzoomedFov(cam) - 0.5f;
    static bool was_zoomed = false;
    if (own && zoomed != was_zoomed) {
        LOGR("aim: %s, angle %.1f of %.1f, %u players", zoomed ? "zoomed" : "unzoomed", F(cam + kCamFov),
             UnzoomedFov(cam), kernel::R32(kPlayers));
        was_zoomed = zoomed;
    }
    if (own) mouse::SetZoomed(zoomed);
    float dx, dy;
    if (!TakeCounts(player, dx, dy)) return;
    float step = Step() * ZoomScale();
    float yaw0 = F(player + kYaw), pitch0 = F(player + kPitch);
    if (!std::isfinite(yaw0) || !std::isfinite(pitch0)) return;
    float yaw = std::remainder(yaw0 - dx * step, 360.0f);
    if (yaw < 0) yaw += 360.0f;  // the game keeps it in [0, 360)
    float pitch = pitch0 - Vertical(dy) * step;
    float limit = F(kPitchLimit);
    if (!(limit > 0 && limit <= 90)) limit = 80.0f;
    pitch = std::clamp(pitch, -limit, limit);
    SetF(player + kYaw, yaw);
    SetF(player + kPitch, pitch);
    static const bool trace = std::getenv("XBE_TRACE_AIM") != nullptr;
    if (trace) LOGR("aim: mouse %.0f %.0f yaw %.3f -> %.3f pitch %.3f -> %.3f zoom %.3f", dx, dy, yaw0, yaw, pitch0, pitch,
                    ZoomScale());
}

}  // namespace

// MountedGunLook(): EAX the gun's holder (+0x7C its angles), EBX the stick's look; state 0x80
X86_EXTERN_FUNC(__imp__sub_00258A20);
X86_FUNC(sub_00258A20) {
    if (Valid(ctx.eax, 0x80)) TurnMounted(kernel::R32(ctx.eax + 0x7C), ZoomScale());
    __imp__sub_00258A20(ctx, base);
}

// MountedViewLook(): EBX the thing looked through; state 0x40. The player's own angles turn, the game then holds them
// within the thing's limits around its heading.
X86_EXTERN_FUNC(__imp__sub_0025D270);
X86_FUNC(sub_0025D270) {
    uint32_t player = kernel::R32(kPlayer);
    float dx, dy;
    if (player && TakeTurn(player, dx, dy)) {
        float step = Step() * ZoomScale();
        SetF(player + kYaw, F(player + kYaw) - dx * step);
        SetF(player + kPitch, F(player + kPitch) - Vertical(dy) * step);
    }
    __imp__sub_0025D270(ctx, base);
}

namespace {

// The tank (Machine Wars, state 0x400, its step 0xD2470) by the player's seat in it (0x17A0A0). The driver's turret
// (seat 0 of a vehicle with flag 1) adds the stick to the player's angles, the barrel's pitch lower is up and held
// within -28 to -4. The gunner's (seats 3 and 4) is turned by the vehicle (0x17A440) at a capped speed from a ramped,
// squared stick: there the mouse turns the turret's own angles before the vehicle clamps them.
constexpr uint32_t kTankState = 0x400, kHandles = 0x72F5E8;
enum class TankSeat { None, Driver, Gunner };
TankSeat g_tank_seat = TankSeat::None;  // in the local player's step, their seat

uint32_t Vehicle(uint32_t parts) {
    uint32_t handle = Valid(parts, 0xC0C) ? kernel::R32(parts + 0xC08) : 0;
    uint32_t table = kernel::R32(kHandles);
    if (!handle || !table) return 0;
    uint32_t vehicle = table + (handle >> 22) * 0x2E8;
    return Valid(vehicle, 0x80) && kernel::R32(vehicle + 0x2C) == handle ? vehicle : 0;
}

TankSeat SeatOf(X86Context& ctx, uint32_t player) {
    if (kernel::R32(player + kState) != kTankState) return TankSeat::None;
    uint32_t body = kernel::R32(player + 0x14);
    uint32_t parts = Valid(body, 0x80) ? kernel::R32(body + 0x7C) : 0;
    uint32_t vehicle = Vehicle(parts);
    if (!vehicle || kernel::CallGuest(ctx, 0x17A000, {parts})) return TankSeat::None;
    uint32_t seat = kernel::CallGuest(ctx, 0x17A0A0, {parts});
    uint32_t info = kernel::R32(vehicle + 0x7C);
    if (seat == 0 && Valid(info, 0x5C) && (kernel::R32(info + 0x58) & 1)) return TankSeat::Driver;
    return seat == 3 || seat == 4 ? TankSeat::Gunner : TankSeat::None;
}

}  // namespace

// VehicleTurrets(EAX the seat, vehicle, input): the seat's turret, [vehicle+0x7C]+0x144 16 bytes a turret, turned by
// the input then held within its limits. A gunner's seat (+0x30 set) turns it by the input's rates; a driver's turret
// eases toward where the driver looks, by its speed (+0x5C) times the frame's share of a second (0x12660). With the
// mouse the driver's turret is where the view is: the speed is one frame's for the update.
X86_EXTERN_FUNC(__imp__sub_0017A440);
X86_FUNC(sub_0017A440) {
    uint32_t turret = 0;
    if (g_tank_seat != TankSeat::None && Valid(ctx.eax, 0x34)) {
        uint32_t vehicle = kernel::R32(ctx.esp + 4);
        uint32_t info = Valid(vehicle, 0x80) ? kernel::R32(vehicle + 0x7C) : 0;
        uint32_t index = kernel::R32(ctx.eax + 0x2C);
        if (Valid(info, 0x168) && index < kernel::R32(info + 0x164) && index < 64)
            turret = kernel::R32(info + 0x144 + index * 16);
        if (!Valid(turret, 0x60)) turret = 0;
    }
    bool gunner_seat = turret && kernel::R32(ctx.eax + 0x30);
    if (turret && g_tank_seat == TankSeat::Gunner && gunner_seat) TurnMounted(turret, ZoomScale());
    uint32_t speed = 0;
    float frame = F(0x4E794C) / float(std::max<int32_t>(int32_t(kernel::R32(0x4E8CEC)), 1));
    if (turret && g_tank_seat == TankSeat::Driver && !gunner_seat && PlayerKeyboard(kernel::R32(kPlayer)) && frame > 0) {
        speed = kernel::R32(turret + 0x5C);
        SetF(turret + 0x5C, 1.0f / frame);
    }
    __imp__sub_0017A440(ctx, base);
    if (speed) kernel::W32(turret + 0x5C, speed);
}

// MountedMove(): each mounted state's own step (0xD5C70); state 0x10000 is a two-part camera, a base and a head each
// with its limits, the left stick turning the first and the right stick the second (0xD0E30, reached by a jump): the
// mouse turns the head before that step clamps it
// the keyboard and mouse player's mounted state while its step runs, for the pad poll's thread: 0x100 the Goliath
static std::atomic<uint32_t> g_mounted_state{0};
static std::atomic<int64_t> g_mounted_at{0};  // steady clock ticks of the last step

X86_EXTERN_FUNC(__imp__sub_000D5C70);
X86_FUNC(sub_000D5C70) {
    uint32_t player = kernel::R32(kPlayer);
    if (Own(player)) {
        g_mounted_state = kernel::R32(player + kState);
        g_mounted_at = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    TankSeat seat = player && player == LocalPlayer() && !Paused() ? SeatOf(ctx, player) : TankSeat::None;
    if (Own(player)) {
        static uint32_t last_state = 0;
        static TankSeat last_seat = TankSeat::None;
        uint32_t state = kernel::R32(player + kState);
        if (state != last_state || seat != last_seat)
            LOGR("aim: mounted state %X, %s", state,
                 seat == TankSeat::Driver ? "a vehicle's driver" : seat == TankSeat::Gunner ? "a vehicle's gunner" : "no seat");
        last_state = state;
        last_seat = seat;
    }
    if (seat == TankSeat::Gunner && Own(player)) mouse::Aiming();
    if (seat == TankSeat::Driver) {
        float dx, dy;
        float yaw = F(player + kYaw), pitch = F(player + kPitch);
        if (TakeTurn(player, dx, dy) && std::isfinite(yaw) && std::isfinite(pitch)) {
            float step = Step() * ZoomScale();
            SetF(player + kYaw, yaw - dx * step);
            SetF(player + kPitch, pitch + Vertical(dy) * step);  // the barrel: lower is up
        }
    }
    if (seat != TankSeat::None) {
        g_tank_seat = seat;
        __imp__sub_000D5C70(ctx, base);
        g_tank_seat = TankSeat::None;
        return;
    }
    // a vehicle's step: the mouse's counts added to the player's angles first, kept or not after it
    float yaw_was = 0, moved = 0;
    VehicleTurn* turn = nullptr;
    if (player && player == LocalPlayer() && !TurnedHere(kernel::R32(player + kState)) && !ViewTurns(player) &&
        !Paused()) {
        turn = &g_vehicle_turn[VehicleKey(player)];
        float dx, dy;
        if (*turn != VehicleTurn::Stick) {
            if (Own(player)) mouse::Aiming();
            yaw_was = F(player + kYaw);
            float pitch = F(player + kPitch);
            if (std::isfinite(yaw_was) && std::isfinite(pitch) && TakeCounts(player, dx, dy)) {
                float step = Step() * ZoomScale();
                moved = -dx * step;
                SetF(player + kYaw, yaw_was + moved);
                SetF(player + kPitch, pitch - Vertical(dy) * step);
            }
        }
    }
    MarkVehicle(player);
    if (player && kernel::R32(player + kState) == 0x10000) {
        uint32_t body = kernel::R32(player + 0x14);
        uint32_t parts = Valid(body, 0x80) ? kernel::R32(body + 0x7C) : 0;
        uint32_t head = Valid(parts, 0xC1C) ? kernel::R32(parts + 0xC18) : 0;
        if (Valid(head, 0x80)) TurnMounted(kernel::R32(head + 0x7C), ZoomScale());
    }
    __imp__sub_000D5C70(ctx, base);
    // learned on a turn big enough to tell: the step keeps it (it may wrap the angle or add a little stick)
    if (turn && *turn == VehicleTurn::Unknown && std::abs(moved) >= 0.5f) {
        float kept = std::remainder(F(player + kYaw) - yaw_was, 360.0f);
        *turn = std::abs(kept - moved) < std::abs(moved) * 0.25f ? VehicleTurn::Direct : VehicleTurn::Stick;
        LOGI("aim: vehicle state %X turns %s", kernel::R32(player + kState),
             *turn == VehicleTurn::Direct ? "1:1 with the mouse" : "by the stick, the mouse tilts it");
    }
}

// MoveVectors(): the frame's movement from the camera yaw
// the ground covered between builds (position +0xB4, height +0xB8 left out) over a second of a held full move, straight
// or diagonal, logged once each: the diagonal's should match the straight one's
static void LogMoveSpeed(float x, float y) {
    static uint32_t last_player = 0;
    static float last[2] = {};
    static int kind = -1, frames = 0, logged = 0;
    static float covered = 0;
    uint32_t player = LocalPlayer();
    if (!player || logged == 3) return;
    float pos[2] = {std::bit_cast<float>(kernel::R32(player + 0xB4)), std::bit_cast<float>(kernel::R32(player + 0xBC))};
    float d = player == last_player ? std::hypot(pos[0] - last[0], pos[1] - last[1]) : 0.0f;
    last_player = player;
    last[0] = pos[0], last[1] = pos[1];
    float ax = std::abs(x), ay = std::abs(y);
    int now = std::hypot(x, y) < 0.95f ? -1 : (ax < 0.05f || ay < 0.05f) ? 0 : std::abs(ax - ay) < 0.1f ? 1 : -1;
    if (now != kind || d > 50.0f) {
        kind = now, frames = 0, covered = 0;
        return;
    }
    if (kind < 0 || (logged & (1 << kind))) return;
    covered += d;
    if (++frames == 60) {
        LOGR("move: %s, %.3f a build", kind ? "diagonal" : "straight", covered / frames);
        logged |= 1 << kind;
    }
}

// The move (ECX: ahead, across, the stick held to a circle by 0x25CE80) drives the run animations, forward and strafe,
// whose blend moves the player: a diagonal at 0.71 of each covered less ground than either alone. For the build the
// move is scaled by (|x| + |y|) / length, so a full diagonal is 1 on both and every direction runs at the same speed.
X86_EXTERN_FUNC(__imp__sub_000D2B80);
X86_FUNC(sub_000D2B80) {
    ApplyMouse();
    uint32_t move = ctx.ecx;
    float x = std::bit_cast<float>(kernel::R32(move)), y = std::bit_cast<float>(kernel::R32(move + 4));
    float length = std::sqrt(x * x + y * y);
    bool scaled = std::isfinite(length) && length > 1e-4f && x != 0 && y != 0;
    if (scaled) {
        float k = (std::abs(x) + std::abs(y)) / length;
        kernel::W32(move, std::bit_cast<uint32_t>(x * k));
        kernel::W32(move + 4, std::bit_cast<uint32_t>(y * k));
    }
    __imp__sub_000D2B80(ctx, base);
    if (scaled) {
        kernel::W32(move, std::bit_cast<uint32_t>(x));
        kernel::W32(move + 4, std::bit_cast<uint32_t>(y));
    }
    LogMoveSpeed(x, y);
}

// LookUpdate(look input, move, look out): with the mouse, auto lookahead would pull the pitch level while walking;
// it is off for the update, the player's setting kept
X86_EXTERN_FUNC(__imp__sub_0025D520);
X86_FUNC(sub_0025D520) {
    uint32_t player = LocalPlayer(), settings = 0;
    uint8_t saved = 0;
    if (player && PlayerKeyboard(player)) {
        settings = kernel::R32(player + kControls);
        if (settings) {
            uint8_t* b = static_cast<uint8_t*>(kernel::HostPtr(settings + 2));
            saved = *b & 8;
            *b &= uint8_t(~8);
        }
    }
    __imp__sub_0025D520(ctx, base);
    if (settings) {
        uint8_t* b = static_cast<uint8_t*>(kernel::HostPtr(settings + 2));
        *b = uint8_t((*b & ~8) | saved);
    }
}

namespace game {

// a session's frame of this machine's mouse (net/lockstep.h): the counts with its sensitivity, up and down share and
// inversion applied, so the machines turn by the same numbers
bool TakeMouseTurn(float& dx, float& dy) {
    float x, y;
    if (!mouse::TakeMotion(x, y)) return false;
    const Options& o = Opt();
    float k = float(o.mouse_sensitivity) / 100.0f;
    dx = x * k;
    dy = y * k * float(o.mouse_vertical) / 100.0f * (o.invert_mouse ? -1.0f : 1.0f);
    return true;
}

// in the Goliath (mounted state 0x100) its minigun is the left trigger's: the right button fires it. A step within the
// last quarter second counts, so leaving it gives the button back to the aim
bool RightButtonFiresLeftTrigger() {
    using namespace std::chrono;
    auto at = steady_clock::time_point(steady_clock::duration(g_mounted_at.load()));
    return g_mounted_state == kGoliathState && steady_clock::now() - at < milliseconds(250);
}

}  // namespace game
