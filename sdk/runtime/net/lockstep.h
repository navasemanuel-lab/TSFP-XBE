#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/input.h"

// Online co-op in lockstep: two PCs each run the whole game, a split screen co-op of two players, and swap their
// players' controls every frame. Each waits for the other's controls before a frame starts, so given the same inputs
// both games play out the same: nothing the game does is sent, only what the players press. That holds only for a
// game that does the same thing with the same inputs on any machine, so a session puts every clock the game can read
// on the game's own frames (the time a frame starts is the frame count over 60), mixes its sound a frame at a time,
// dates files alike and has both machines start from the host's saves. Each machine then shows its own player full
// screen (the game's side, game/coop_online.cpp).
//
// A session is set on the command line: --host [port] waits for player 2 (this machine is player 1, port 7777 unless
// given), --join <address>[:port] joins one as player 2; --net-pass <text> a password the join must give,
// --net-delay <frames> the input delay instead of one from the measured round trip. It begins before the game boots
// (Begin), in a small window that says what is happening and can be cancelled.
namespace lockstep {

constexpr int kPlayers = 2;
constexpr uint16_t kDefaultPort = 7777;

// a session was asked for on the command line
bool Requested();
// window thread, before boot: the session's setup (peer, build check, the host's saves to the joiner, round trip,
// the host's settings); data_dir is the save folder, the joiner's becomes one of its own holding the host's. false:
// no session could be made or it was cancelled, the reason shown; the game should quit.
bool Begin(const std::vector<uint8_t>& xbe, std::filesystem::path& data_dir);
// a session is running: the clocks are the frames', the pads the network's
bool Active();
int LocalPort();  // this machine's player's game port: 0 the host, 1 the joiner
int RemotePort();
// the other game runs on this PC (a test of two copies): the host plays on the keyboard and mouse, player 2 on the
// controllers
bool PeerOnThisPC();

// The game's settings that change how it plays out, the host's in a session: each a key and its value, the game sets
// and reads them (GameInfo hooks). Up to 32.
struct Pin {
    const char* key;
    bool* flag;
    uint32_t* value;
    uint32_t Get() const { return flag ? uint32_t(*flag) : *value; }
    void Set(uint32_t v) const { flag ? void(*flag = v != 0) : void(*value = v); }
};
void Pins(const Pin* pins, int count);
// What the game reads besides the disc, which has to be alike on both machines (the mods switched on): a hash, and
// what it is in words for the player turned away when it differs
void Content(uint64_t hash, const std::string& what);

// the game's frame starts (its frame clock, main thread): this machine's controls go out for a frame ahead, the
// frame's controls of both players are waited for, the clocks step a frame, the sound mixes a frame
void FrameStart();
uint64_t Frame();

// this machine's player's controls as the game makes them (keyboard, mouse buttons, controllers), read once a
// frame as it starts, the mouse's turn since the last (in the game's units, this machine's sensitivity applied) and
// whether the keyboard and mouse are what the player uses
using Capture = input::Pad (*)(float& turn_x, float& turn_y, bool& keyboard);
void SetCapture(Capture capture);
// the controls of game port `port` for the frame running; ports past the players are not plugged in
input::Pad PortPad(int port);
// the mouse's turn of that port's player for the frame running, given once a frame (0 after): both machines turn
// both players alike
bool PortAim(int port, float& dx, float& dy);
bool PortKeyboard(int port);  // that port's player plays on the keyboard and mouse

// 100 ns since boot, the same at the same point of the game on both machines: the frames' time, a microsecond more
// each time a thread reads it within a frame (so a wait on the clock ends)
uint64_t Now100ns();

// the game's state hashed every second and compared, a difference logged with the frame (and each 64 KB of it to
// logs/desync-<frame>.txt for a look at what parted)
void HashRange(uint32_t va, uint32_t size);
void HashSkip(uint32_t va, uint32_t size);  // a part of those that differs between machines and does not matter

// a line for the screen while the session has something to say (the other player is late, gone, or the games
// drifted apart), empty otherwise
std::string Notice();
std::string Status();  // the session as one line: peer, delay, round trip

// restarts the game into a session (spec: "host:<port>" or "join:<address>:<port>", empty to leave one), the
// password and the delay kept; false when the new process could not be started, else this one ends
bool Restart(const std::string& spec, const std::string& pass);
void Bye();  // the game closes: the other is told

}  // namespace lockstep
