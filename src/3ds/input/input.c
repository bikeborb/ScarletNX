/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Keys are Azahar's defaults (citra_qt/configuration/config.cpp, GPL-2.0-or-later, Copyright 2014-2026 Citra
   Emulator Project / Azahar Emulator Project): A, S, Z and X for A, B, X and Y, T, G, F and H for the D-pad, Q and W
   for L and R, M and N for Start and Select, O and P for Debug and GPIO14, the arrows for the circle pad with D
   halving it. A gamepad's face buttons go by position, as the 3DS's are: A on the right, B at the bottom. */
#include "3ds/input/input.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>

#define PRESS_NS       300000000u /* how long a scripted press is held, as in the Switch runtime */
#define STICK_DEADZONE 0.1f
#define GAMEPAD_RESCAN 64u /* reads between looks for a gamepad while none is open */
#define DIAGONAL       0.707106781f

typedef struct {
    uint64_t at_ns;
    uint32_t buttons;
} Press;

static struct Input {
    bool up, gamepads;
    uint64_t start_ns;
    Press* script;
    size_t presses;
    SDL_Gamepad* gamepad;
    unsigned rescan;
} in;

/* Checks the script, and keeps its presses when store is set */
static bool parse(const char* script, bool store) {
    size_t cap = 0;
    for (const char* p = script; *p;) {
        char* end;
        unsigned long long ms = strtoull(p, &end, 10);
        if (end == p || *end != ':') return false;
        const char* hex = end + 1;
        unsigned long long buttons = strtoull(hex, &end, 16);
        if (end == hex || (*end && *end != ',') || buttons > SNX_CTR_PAD_BUTTONS || ms > UINT64_MAX / 1000000u)
            return false;
        p = *end ? end + 1 : end;
        if (!store) continue;
        if (in.presses == cap) {
            cap = cap ? cap * 2 : 16;
            Press* grown = realloc(in.script, cap * sizeof *grown);
            if (!grown) return false;
            in.script = grown;
        }
        in.script[in.presses++] = (Press){(uint64_t)ms * 1000000u, (uint32_t)buttons};
    }
    return true;
}
bool snx_ctr_input_script_valid(const char* script) { return parse(script, false); }
bool snx_ctr_input_init(const char* script) {
    if (in.up) return true;
    if (script && !parse(script, true)) {
        snx_ctr_input_free();
        return false;
    }
    in.gamepads = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    in.start_ns = SDL_GetTicksNS();
    in.up = true;
    return true;
}
void snx_ctr_input_free(void) {
    if (in.gamepad) SDL_CloseGamepad(in.gamepad);
    if (in.gamepads) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    free(in.script);
    in = (struct Input){0};
}

/* The first gamepad, opened while it stays connected, and looked for again now and then while there is none */
static SDL_Gamepad* gamepad(void) {
    if (!in.gamepads) return NULL;
    if (in.gamepad && !SDL_GamepadConnected(in.gamepad)) {
        SDL_CloseGamepad(in.gamepad);
        in.gamepad = NULL;
    }
    if (!in.gamepad && !in.rescan--) {
        in.rescan = GAMEPAD_RESCAN;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count) in.gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    return in.gamepad;
}
static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) { return SDL_GetGamepadAxis(g, a) / 32767.0f; }

void snx_ctr_input_read(SnxCtrInput* input) {
    *input = (SnxCtrInput){0};
    if (!in.up) return;
    uint64_t now = SDL_GetTicksNS() - in.start_ns;
    for (size_t i = 0; i < in.presses; ++i)
        if (now >= in.script[i].at_ns && now - in.script[i].at_ns < PRESS_NS) input->buttons |= in.script[i].buttons;
    /* the keyboard's state and the gamepad's change as SDL's events are pumped, which the presenter drains */
    if (SDL_WasInit(SDL_INIT_EVENTS)) SDL_PumpEvents();
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        static const struct {
            SDL_Scancode key;
            uint32_t button;
        } keys[] = {{SDL_SCANCODE_A, SNX_CTR_PAD_A},     {SDL_SCANCODE_S, SNX_CTR_PAD_B},
                    {SDL_SCANCODE_Z, SNX_CTR_PAD_X},     {SDL_SCANCODE_X, SNX_CTR_PAD_Y},
                    {SDL_SCANCODE_T, SNX_CTR_PAD_UP},    {SDL_SCANCODE_G, SNX_CTR_PAD_DOWN},
                    {SDL_SCANCODE_F, SNX_CTR_PAD_LEFT},  {SDL_SCANCODE_H, SNX_CTR_PAD_RIGHT},
                    {SDL_SCANCODE_Q, SNX_CTR_PAD_L},     {SDL_SCANCODE_W, SNX_CTR_PAD_R},
                    {SDL_SCANCODE_M, SNX_CTR_PAD_START}, {SDL_SCANCODE_N, SNX_CTR_PAD_SELECT},
                    {SDL_SCANCODE_O, SNX_CTR_PAD_DEBUG}, {SDL_SCANCODE_P, SNX_CTR_PAD_GPIO14}};
        const bool* state = SDL_GetKeyboardState(NULL);
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
            if (state[keys[i].key]) input->buttons |= keys[i].button;
        float x = (float)state[SDL_SCANCODE_RIGHT] - (float)state[SDL_SCANCODE_LEFT];
        float y = (float)state[SDL_SCANCODE_UP] - (float)state[SDL_SCANCODE_DOWN];
        float scale = (x != 0 && y != 0 ? DIAGONAL : 1.0f) * (state[SDL_SCANCODE_D] ? 0.5f : 1.0f);
        input->circle_x = x * scale;
        input->circle_y = y * scale;
    }
    SDL_Gamepad* g = gamepad();
    if (!g) return;
    static const struct {
        SDL_GamepadButton pad;
        uint32_t button;
    } buttons[] = {
        {SDL_GAMEPAD_BUTTON_EAST, SNX_CTR_PAD_A},          {SDL_GAMEPAD_BUTTON_SOUTH, SNX_CTR_PAD_B},
        {SDL_GAMEPAD_BUTTON_NORTH, SNX_CTR_PAD_X},         {SDL_GAMEPAD_BUTTON_WEST, SNX_CTR_PAD_Y},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, SNX_CTR_PAD_UP},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SNX_CTR_PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SNX_CTR_PAD_LEFT},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SNX_CTR_PAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SNX_CTR_PAD_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SNX_CTR_PAD_R},
        {SDL_GAMEPAD_BUTTON_START, SNX_CTR_PAD_START},     {SDL_GAMEPAD_BUTTON_BACK, SNX_CTR_PAD_SELECT}};
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (SDL_GetGamepadButton(g, buttons[i].pad)) input->buttons |= buttons[i].button;
    /* the left stick, SDL's down positive turned up, past a small deadzone and scaled back to the full range */
    float x = axis(g, SDL_GAMEPAD_AXIS_LEFTX), y = -axis(g, SDL_GAMEPAD_AXIS_LEFTY), r = sqrtf(x * x + y * y);
    if (r > STICK_DEADZONE) {
        float scale = ((r < 1.0f ? r : 1.0f) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE) / r;
        input->circle_x = x * scale;
        input->circle_y = y * scale;
    }
}
