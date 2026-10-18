/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The host's input as the 3DS's HID module samples it: the keyboard with Azahar's default keys, the first gamepad,
   and an unattended run's scripted presses. */
#ifndef SNX_CTR_INPUT_H
#define SNX_CTR_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* HID's PadState bits for the buttons (the circle pad's direction bits are HID's own, from its position) */
enum {
    SNX_CTR_PAD_A = 1u << 0,
    SNX_CTR_PAD_B = 1u << 1,
    SNX_CTR_PAD_SELECT = 1u << 2,
    SNX_CTR_PAD_START = 1u << 3,
    SNX_CTR_PAD_RIGHT = 1u << 4,
    SNX_CTR_PAD_LEFT = 1u << 5,
    SNX_CTR_PAD_UP = 1u << 6,
    SNX_CTR_PAD_DOWN = 1u << 7,
    SNX_CTR_PAD_R = 1u << 8,
    SNX_CTR_PAD_L = 1u << 9,
    SNX_CTR_PAD_X = 1u << 10,
    SNX_CTR_PAD_Y = 1u << 11,
    SNX_CTR_PAD_DEBUG = 1u << 12,
    SNX_CTR_PAD_GPIO14 = 1u << 13,
    SNX_CTR_PAD_BUTTONS = 0x3fffu
};

typedef struct {
    uint32_t buttons;         /* SNX_CTR_PAD_* */
    float circle_x, circle_y; /* the circle pad, -1 to 1 with right and up positive, within the unit circle */
    bool touch;               /* the bottom screen pressed at touch_x, touch_y, 0 to 1 from its top left */
    float touch_x, touch_y;
} SnxCtrInput;

/* script: NULL, or "ms:hex,ms:hex,..." as the Switch runtime's --script takes it, each press of the hex PadState
   buttons held for 300 ms from that many milliseconds after init. False when the script does not parse */
bool snx_ctr_input_init(const char* script);
bool snx_ctr_input_script_valid(const char* script);
/* The input now: nothing pressed before init, and only the script without a window or a gamepad */
void snx_ctr_input_read(SnxCtrInput* input);
void snx_ctr_input_free(void);
#endif
