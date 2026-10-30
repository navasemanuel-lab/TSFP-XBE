// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "net/lockstep.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};
int g_reserved = -1;

// A controller's gyro, after JibbSmart's "player space": every sample of the sensor (a few hundred a second) is turned
// into degrees, the turn about the world's up (from the accelerometer's gravity) for left and right, so the controller
// can be held at any tilt, and about its own side axis for up and down. SDL's axes: x right, y up, z toward the player,
// turns counterclockwise positive; a controller at rest reads gravity as +y. The drift is the mean of the gyro over a
// second the controller lies still (the rate and the gravity steady), taken out from then on.
struct Motion {
    bool on = false;
    uint64_t last_ns = 0;
    float up[3] = {0, 1, 0};  // the world's up, in the controller's axes
    float bias[3] = {};       // rad/s
    float sum[3] = {};        // the still window's
    uint32_t still = 0;
    uint64_t still_from = 0;
    float accel = 0;          // the last gravity's size, m/s^2
    double yaw = 0, pitch = 0;  // degrees turned since the last take, left and up positive
};
Motion g_motion[4];

void MotionSample(Motion& m, SDL_SensorType type, const float* d, uint64_t ns) {
    constexpr float kDegrees = 57.2957795f;
    if (type == SDL_SENSOR_ACCEL) {
        float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (n < 1.0f) return;
        // a still controller's gravity, smoothed: shakes and moves weigh little
        float change = std::fabs(n - m.accel);
        m.accel = n;
        if (change > 0.4f) m.still = 0;
        float len = 0;
        for (int i = 0; i < 3; i++) {
            m.up[i] += (d[i] / n - m.up[i]) * 0.05f;
            len += m.up[i] * m.up[i];
        }
        len = std::sqrt(len);
        if (len > 1e-3f)
            for (float& v : m.up) v /= len;
        return;
    }
    if (type != SDL_SENSOR_GYRO) return;
    float dt = m.last_ns && ns > m.last_ns ? float(double(ns - m.last_ns) / 1e9) : 0.0f;
    m.last_ns = ns;
    // the drift: a second of steady readings, the rate within 1.5 degrees a second of their mean so far
    bool steady = true;
    if (m.still) {
        for (int i = 0; i < 3; i++)
            if (std::fabs(d[i] - m.sum[i] / float(m.still)) * kDegrees > 1.5f) steady = false;
    }
    if (!steady) m.still = 0;
    if (!m.still) {
        m.sum[0] = m.sum[1] = m.sum[2] = 0;
        m.still_from = ns;
    }
    for (int i = 0; i < 3; i++) m.sum[i] += d[i];
    m.still++;
    if (ns - m.still_from > 1000000000ull && m.still >= 20) {
        for (int i = 0; i < 3; i++) m.bias[i] = m.sum[i] / float(m.still);
        m.still = 0;
    }
    if (dt <= 0 || dt > 0.05f) return;
    float g[3] = {d[0] - m.bias[0], d[1] - m.bias[1], d[2] - m.bias[2]};
    // left and right about the world's up, eased toward the turn about the controller's own up and forward axes
    float world = g[1] * m.up[1] + g[2] * m.up[2];
    float local = std::sqrt(g[1] * g[1] + g[2] * g[2]);
    float yaw = std::copysign(std::min(std::fabs(world) * 1.41f, local), world);
    m.yaw += double(yaw * kDegrees * dt);
    m.pitch += double(g[0] * kDegrees * dt);
}

void StartMotion(int port) {
    g_motion[port] = {};
    SDL_Gamepad* pad = g_pads[port];
    if (!pad || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) return;
    bool gyro = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    g_motion[port].on = gyro;
    LOGI("input: controller %d has a gyro%s", port + 1, gyro ? "" : ", which would not start");
}

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::end(g_pads);
    for (int i = 0; i < 4 && slot == std::end(g_pads); i++)
        if (!g_pads[i] && i != g_reserved) slot = &g_pads[i];
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
    StartMotion(int(slot - g_pads));
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        g_motion[i] = {};
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
            for (int i = 0; i < 4; i++)
                if (g_pads[i] && g_motion[i].on && SDL_GetGamepadID(g_pads[i]) == e.gsensor.which)
                    MotionSample(g_motion[i], SDL_SensorType(e.gsensor.sensor), e.gsensor.data, e.gsensor.sensor_timestamp);
    }
}

}  // namespace

void Init() {
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    if (lockstep::Active()) return lockstep::PortPad(port);  // a co-op session: the frame's controls, both machines'
    return PollLocal(port);
}

Pad PollLocal(int port) {
    Pad p = Read(port);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

Pad Read(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    return p;
}

Family PadFamily(int port) {
    std::lock_guard lock(g_lock);
    SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr;
    if (!pad) return Family::None;
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return Family::Xbox;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4: return Family::PlayStation4;
        case SDL_GAMEPAD_TYPE_PS5: return Family::PlayStation5;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Family::Switch;
        default: return Family::Other;
    }
}

void ReservePort(int port) {
    std::lock_guard lock(g_lock);
    g_reserved = port;
    if (port < 0 || port >= 4 || !g_pads[port]) return;
    for (int i = 0; i < 4; i++) {
        if (i == port || g_pads[i]) continue;
        g_pads[i] = g_pads[port];
        g_pads[port] = nullptr;
        g_motion[i] = g_motion[port];
        g_motion[port] = {};
        LOGI("input: controller %d moved to port %d, port %d is the keyboard's", port + 1, i + 1, port + 1);
        return;
    }
}

bool TakeGyro(int port, float& yaw, float& pitch) {
    std::lock_guard lock(g_lock);
    UpdatePads();
    if (port < 0 || port >= 4 || !g_motion[port].on) return false;
    Motion& m = g_motion[port];
    yaw = float(m.yaw);
    pitch = float(m.pitch);
    m.yaw = m.pitch = 0;
    return true;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
