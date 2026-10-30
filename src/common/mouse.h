#pragma once
#include <cstdint>

// the mouse: raw movement for aiming, the cursor locked in the window, a pointer in the menus (mouse.cpp)
namespace mouse {

void Start(void* hwnd);  // GameInfo::window
// window thread: a message the mouse takes; true returns result instead of the host's handling
bool Message(unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void SetPanel(bool open);               // the settings panel opened or closed: the cursor free while it is open
void KeepLocked();                      // each pad poll: the lock put back if something took it
void Aiming();                          // game thread, each look update: the view takes the mouse now
bool InPlay();                          // the view took the mouse lately: in play, not a menu or MapMaker
bool TakeMotion(float& dx, float& dy);  // counts moved since the last take, right and down positive
void Inject(int dx, int dy);            // input scripts
bool ButtonsBlocked();  // the buttons are not the game's: the cursor is free, or the click that locked it is held
int WheelButton();      // a wheel notch in play as a short press: 1 up, -1 down, 0 none
// game thread, each frame of a vehicle or gun that turns by its own stick step (a tank, a railbot): the mouse moves
// the right stick there, its speed the stick's tilt
void InVehicle();
bool VehicleStick(int16_t& rx, int16_t& ry);  // pad poll: the stick the mouse moves, false when it has not moved
void SetZoomed(bool on);  // game thread: the view is zoomed in, the wheel zooms rather than changing weapons
bool Zoomed();

// the menus
struct Pointer {
    float x = 0, y = 0;  // 0-1 across the shown frame
    bool valid = false, left = false, right = false;
    int wheel = 0;       // notches since the last take, up positive
};
void InMenu();                    // game thread, each tick of a menu that takes the pointer
void InTerminal();                // game thread, each frame a terminal's pointer takes the mouse's counts
bool TerminalMode();              // its buttons are A and B, not the triggers
void InGameCursor();              // game thread, each frame the game draws a cursor of its own (MapMaker)
bool GameCursorMode();            // its cursor follows the mouse: the clicks are A and B, the system arrow hidden
bool MenuMode();                  // a menu has the pointer: its clicks are not the pad's triggers
Pointer MenuPointer(bool take);   // take: the wheel notches too
void TestPointer(float x, float y, bool left, bool right);  // input scripts: in place of the cursor for a moment

// typing into the game's on-screen keyboards
void SetTextEntry(bool on);  // game thread: a keyboard screen is up, typed characters are kept and keys are not buttons
bool TextEntry();
int TakeChar();              // the next typed character, 0 none; '\b' Backspace, '\r' Enter
void TypeChars(const char* s);  // input scripts

}  // namespace mouse
