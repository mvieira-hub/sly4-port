// The window's controller: the first SDL gamepad plugged in (any SDL knows: DualShock,
// DualSense, Xbox...) and the keyboard, read once a frame into cellPad's state
// (kernel/hle_cell_pad.cpp); the game's vibration goes back to the gamepad.
//
// Keyboard: arrows = D-pad, W A S D = left stick, I J K L = right stick, Enter = Start,
// Space = Select, X = Cross, C = Circle, Z = Square, V = Triangle, Q / E = L1 / R1,
// R / T = L2 / R2, F / G = L3 / R3.
#pragma once

#include <cstdint>

#include <SDL3/SDL.h>

namespace sly4 {

class PadInput {
public:
    ~PadInput();
    void handle(const SDL_Event& ev);  // gamepads plugged in and out
    void update(bool focused);         // reads the devices into the pad state; vibration
    const char* gamepad_name() const;

private:
    void open(SDL_JoystickID id);
    SDL_Gamepad* gamepad_ = nullptr;
    SDL_JoystickID id_ = 0;
    uint32_t rumble_serial_ = 0;
};

}  // namespace sly4
