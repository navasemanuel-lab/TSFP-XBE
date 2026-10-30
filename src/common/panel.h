#pragma once
#include <cstdint>

namespace input {
struct Pad;
}

// the settings panel over the game (panel.cpp): F1, or Back and Start together on a pad
namespace panel {

void Start();         // the game window is up and ImGui made: the panel's letters
void Ui(double fps);  // render thread, each frame inside the ImGui frame
// window thread: F1 opens and closes it; true returns result instead of the host's handling
bool Message(unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void Toggle();  // any thread
bool Open();
void ShowCoop();  // opened at the Co-op page (an online game to join wants its address)
// a pad poll, after the keyboard's: the game sees the pad idle, while the panel is open and after it until what
// closed it is let go
bool Holds(int port, const input::Pad& pad);

}  // namespace panel
