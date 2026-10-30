#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// what kind of controller is on a port, for the button names a game shows
enum class Family : uint8_t { None, Xbox, PlayStation4, PlayStation5, Switch, Other };
Family PadFamily(int port);

void Init();  // window thread, before boot
// the controller on that port, then GameInfo::pad; in a co-op session the frame's controls of that port's player
Pad Poll(int port);
Pad PollLocal(int port);  // this machine's, also in a session
Pad Read(int port);  // the controller alone, without GameInfo::pad
void Rumble(int port, uint16_t left, uint16_t right);
// the controller's turn since the last take in degrees, left and up positive, held at any tilt; false without a gyro
bool TakeGyro(int port, float& yaw, float& pitch);
// a port no controller takes (a game's keyboard and mouse player), -1 none; a controller there moves to a free one
void ReservePort(int port);

}  // namespace input
