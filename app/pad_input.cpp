#include "pad_input.h"

#include <algorithm>
#include <cstdlib>

#include "core/log.h"
#include "kernel/hle.h"

namespace sly4 {

namespace {

// DualShock 3 bits (CellPadData words 2 and 3) and pressure slots (words 8-19).
enum : uint16_t {
    kSelect = 0x01, kL3 = 0x02, kR3 = 0x04, kStart = 0x08, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
    kL2 = 0x01, kR2 = 0x02, kL1 = 0x04, kR1 = 0x08, kTriangle = 0x10, kCircle = 0x20, kCross = 0x40, kSquare = 0x80,
};
enum { pRight, pLeft, pUp, pDown, pTriangle, pCircle, pCross, pSquare, pL1, pR1, pL2, pR2 };

struct Button {
    int word;          // 1 or 2 (digital1 / digital2)
    uint16_t bit;
    int pressure;      // slot, or -1
};
constexpr Button kSelectB{1, kSelect, -1}, kL3B{1, kL3, -1}, kR3B{1, kR3, -1}, kStartB{1, kStart, -1},
    kUpB{1, kUp, pUp}, kRightB{1, kRight, pRight}, kDownB{1, kDown, pDown}, kLeftB{1, kLeft, pLeft},
    kL2B{2, kL2, pL2}, kR2B{2, kR2, pR2}, kL1B{2, kL1, pL1}, kR1B{2, kR1, pR1}, kTriangleB{2, kTriangle, pTriangle},
    kCircleB{2, kCircle, pCircle}, kCrossB{2, kCross, pCross}, kSquareB{2, kSquare, pSquare};

void press(PadState& s, const Button& b, uint16_t pressure = 255) {
    (b.word == 1 ? s.digital1 : s.digital2) |= b.bit;
    if (b.pressure >= 0) s.pressure[b.pressure] = std::max(s.pressure[b.pressure], pressure);
}

// SDL axis (-32768..32767, y down) to the PS3's 0..255 (0x80 centred, y down too), with a
// dead zone around the centre.
uint16_t axis(int16_t v) {
    if (std::abs(int(v)) < 7000) return 0x80;
    return uint16_t((int(v) + 32768) >> 8);
}

}  // namespace

PadInput::~PadInput() {
    if (gamepad_) SDL_CloseGamepad(gamepad_);
}

void PadInput::open(SDL_JoystickID id) {
    if (gamepad_) return;
    gamepad_ = SDL_OpenGamepad(id);
    if (!gamepad_) {
        SLY4_WARN("controle: SDL_OpenGamepad: %s", SDL_GetError());
        return;
    }
    id_ = id;
    SLY4_INFO("controle: %s conectado", gamepad_name());
}

const char* PadInput::gamepad_name() const {
    const char* n = gamepad_ ? SDL_GetGamepadName(gamepad_) : nullptr;
    return n ? n : "gamepad";
}

void PadInput::handle(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
        open(ev.gdevice.which);
    } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad_ && ev.gdevice.which == id_) {
        SLY4_INFO("controle: %s desconectado", gamepad_name());
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        // Another one still plugged in takes over.
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            if (n > 0) open(ids[0]);
            SDL_free(ids);
        }
    }
}

void PadInput::update(bool focused) {
    PadState s;
    if (gamepad_) {
        struct Map {
            SDL_GamepadButton from;
            const Button* to;
        };
        static constexpr Map kMap[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, &kCrossB},          {SDL_GAMEPAD_BUTTON_EAST, &kCircleB},
            {SDL_GAMEPAD_BUTTON_WEST, &kSquareB},          {SDL_GAMEPAD_BUTTON_NORTH, &kTriangleB},
            {SDL_GAMEPAD_BUTTON_BACK, &kSelectB},          {SDL_GAMEPAD_BUTTON_START, &kStartB},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, &kL3B},        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, &kR3B},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, &kL1B},     {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, &kR1B},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, &kUpB},           {SDL_GAMEPAD_BUTTON_DPAD_DOWN, &kDownB},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, &kLeftB},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, &kRightB},
        };
        for (const Map& m : kMap)
            if (SDL_GetGamepadButton(gamepad_, m.from)) press(s, *m.to);
        // Triggers are analog: their pressure, and the button past a quarter of the way.
        const int lt = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7;
        const int rt = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7;
        if (lt > 64) press(s, kL2B, uint16_t(std::min(lt, 255)));
        if (rt > 64) press(s, kR2B, uint16_t(std::min(rt, 255)));
        s.left_x = axis(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX));
        s.left_y = axis(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY));
        s.right_x = axis(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTX));
        s.right_y = axis(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHTY));
        // Vibration: the small motor on or off, the large one with its strength.
        const PadRumble r = pad_rumble();
        if (r.serial != rumble_serial_) {
            rumble_serial_ = r.serial;
            SDL_RumbleGamepad(gamepad_, uint16_t(r.large * 257), r.small ? 0xFFFF : 0, r.small || r.large ? 2000 : 0);
        }
    }
    if (focused) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        struct Key {
            SDL_Scancode from;
            const Button* to;
        };
        static constexpr Key kKeys[] = {
            {SDL_SCANCODE_UP, &kUpB},         {SDL_SCANCODE_DOWN, &kDownB},     {SDL_SCANCODE_LEFT, &kLeftB},
            {SDL_SCANCODE_RIGHT, &kRightB},   {SDL_SCANCODE_RETURN, &kStartB},  {SDL_SCANCODE_KP_ENTER, &kStartB},
            {SDL_SCANCODE_SPACE, &kSelectB},  {SDL_SCANCODE_X, &kCrossB},       {SDL_SCANCODE_C, &kCircleB},
            {SDL_SCANCODE_Z, &kSquareB},      {SDL_SCANCODE_V, &kTriangleB},    {SDL_SCANCODE_Q, &kL1B},
            {SDL_SCANCODE_E, &kR1B},          {SDL_SCANCODE_R, &kL2B},          {SDL_SCANCODE_T, &kR2B},
            {SDL_SCANCODE_F, &kL3B},          {SDL_SCANCODE_G, &kR3B},
        };
        for (const Key& m : kKeys)
            if (k[m.from]) press(s, *m.to);
        auto keys_axis = [&](SDL_Scancode neg, SDL_Scancode pos, uint16_t& v) {
            if (k[neg] != k[pos]) v = k[neg] ? 0 : 255;
        };
        keys_axis(SDL_SCANCODE_A, SDL_SCANCODE_D, s.left_x);
        keys_axis(SDL_SCANCODE_W, SDL_SCANCODE_S, s.left_y);
        keys_axis(SDL_SCANCODE_J, SDL_SCANCODE_L, s.right_x);
        keys_axis(SDL_SCANCODE_I, SDL_SCANCODE_K, s.right_y);
    }
    set_pad_state(s, PadSource::Device);
}

}  // namespace sly4
