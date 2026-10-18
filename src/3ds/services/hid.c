/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   hid:USER: the shared page HID writes the pad, touch, accelerometer and gyroscope samples into, and the events it
   signals with each. Ported from Azahar core/hle/service/hid/hid.{h,cpp} (GPL-2.0-or-later, Copyright 2015-2026 Citra
   Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. The samples are paced by the host clock
   rather than CoreTiming, and the host has no motion sensor yet, so the accelerometer and gyroscope read zero. */
#include "3ds/runtime/internal.h"
#include "3ds/board/board.h"
#include "3ds/services/hid.h"
#include "common/utilities/common.h"
#include <math.h>
#include <string.h>
#include "services/hid.swipc.inc"

/* The shared page (SharedMem): a section per device, each an index_reset_ticks pair, the index of the entry written
   last, then the device's entries */
#define PAD                  0x000u /* sliderstate_3d at 0x18, current_state at 0x1c, entries[8] of 16 bytes at 0x28 */
#define TOUCH                0x0a8u /* raw_entry at 0x18, entries[8] of 8 bytes at 0x20 */
#define ACCELEROMETER        0x108u /* raw_entry at 0x18, entries[8] of 6 bytes at 0x20 */
#define GYROSCOPE            0x158u /* raw_entry at 0x18, entries[32] of 6 bytes at 0x20 */
#define SHARED_USED          0x238u
#define PAD_ENTRIES          8u
#define TOUCH_ENTRIES        8u
#define ACCELEROMETER_COUNT  8u
#define GYROSCOPE_COUNT      32u
/* the update rates, in base ARM11 ticks as Azahar schedules them */
#define TICKS_NS(ticks)      ((uint64_t)(ticks) * 1000000000u / SNX_CTR_BASE_CLOCK_HZ)
#define PAD_NS               TICKS_NS(SNX_CTR_BASE_CLOCK_HZ / 234)
#define ACCELEROMETER_NS     TICKS_NS(SNX_CTR_BASE_CLOCK_HZ / 104)
#define GYROSCOPE_NS         TICKS_NS(SNX_CTR_BASE_CLOCK_HZ / 101)
#define MAX_LATE_NS          100000000u /* past this, sampling starts again from now instead of catching up */
#define MAX_CIRCLEPAD_POS    0x9a       /* the circle pad's calibrated limit */
#define CIRCLE_PAD_THRESHOLD (40 * 40)  /* the squared distance past which it presses a direction */
#define TAN30                0.577350269f
#define TAN60                (1 / TAN30)
#define ACCELEROMETER_COEF   512.0f  /* measured on hardware */
#define GYROSCOPE_COEF       14.375f /* GetGyroscopeLowRawToDpsCoefficient on hardware */
#define GYROSCOPE_PARAM_UNIT 6700    /* approximate, from hardware */
enum { CIRCLE_RIGHT = 1u << 28, CIRCLE_LEFT = 1u << 29, CIRCLE_UP = 1u << 30, CIRCLE_DOWN = 1u << 31 };
_Static_assert(PAD + 0x28 + PAD_ENTRIES * 16 == TOUCH && TOUCH + 0x20 + TOUCH_ENTRIES * 8 == ACCELEROMETER &&
                   ACCELEROMETER + 0x20 + ACCELEROMETER_COUNT * 6 == GYROSCOPE &&
                   GYROSCOPE + 0x20 + GYROSCOPE_COUNT * 6 == SHARED_USED,
               "HID shared memory layout");

static void put16(unsigned char* b, uint16_t v) {
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char* b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (i * 8));
}
static uint32_t get32(const unsigned char* b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint32_t shared_physical(const SnxCtrProcess* p) {
    return p->hid.objects[0] ? p->objects[p->hid.objects[0]].data.shared.physical : 0;
}
static void signal_event(SnxCtrProcess* p, uint32_t id) {
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
/* the base ARM11 tick count svcGetSystemTick reads at now_ns */
static uint64_t ticks_at(const SnxCtrProcess* p, uint64_t now_ns) {
    double elapsed = (double)now_ns / 1e9 - p->clock_origin;
    return elapsed > 0 ? (uint64_t)(elapsed * (double)SNX_CTR_BASE_CLOCK_HZ) : 0;
}
/* A section's header: the index of the entry written now, and a new timestamp each time that is entry 0 */
static void section_index(SnxCtrProcess* p, unsigned char* section, uint32_t index, uint64_t now_ns) {
    put32(section + 0x10, index);
    if (index) return;
    memcpy(section + 8, section, 8); /* index_reset_ticks_previous */
    uint64_t ticks = ticks_at(p, now_ns);
    put32(section, (uint32_t)ticks);
    put32(section + 4, (uint32_t)(ticks >> 32));
}
/* GetStickDirectionState */
static uint32_t stick_direction(int x, int y) {
    uint32_t state = 0;
    if (x * x + y * y > CIRCLE_PAD_THRESHOLD) {
        float t = x ? fabsf((float)y / (float)x) : 0;
        if (x && t < TAN60) state |= x > 0 ? CIRCLE_RIGHT : CIRCLE_LEFT;
        if (!x || t > TAN30) state |= y > 0 ? CIRCLE_UP : CIRCLE_DOWN;
    }
    return state;
}

/* UpdatePadCallback */
void snx_ctr_hid_pad(SnxCtrProcess* p, const SnxCtrInput* input, uint64_t now_ns) {
    uint32_t physical = shared_physical(p);
    if (!physical) return;
    unsigned char pad[TOUCH - PAD], touch[ACCELEROMETER - TOUCH];
    if (snx_ctr_phys_read(p->memory, physical + PAD, pad, sizeof pad) != SNX_CTR_MEM_OK ||
        snx_ctr_phys_read(p->memory, physical + TOUCH, touch, sizeof touch) != SNX_CTR_MEM_OK)
        return;
    SnxCtrHidService* h = &p->hid;
    uint32_t state = input->buttons & SNX_CTR_PAD_BUTTONS;
    /* rounded rather than truncated, as on hardware, and averaged with the two samples before */
    int16_t new_x = (int16_t)roundf(input->circle_x * MAX_CIRCLEPAD_POS),
            new_y = (int16_t)roundf(input->circle_y * MAX_CIRCLEPAD_POS);
    int16_t circle_x = (int16_t)((new_x + h->circle_x[0] + h->circle_x[1]) / 3),
            circle_y = (int16_t)((new_y + h->circle_y[0] + h->circle_y[1]) / 3);
    h->circle_x[0] = h->circle_x[1];
    h->circle_x[1] = new_x;
    h->circle_y[0] = h->circle_y[1];
    h->circle_y[1] = new_y;
    state |= stick_direction(circle_x, circle_y);
    put32(pad + 0x1c, state);
    uint32_t index = h->pad_index;
    h->pad_index = (index + 1) % PAD_ENTRIES;
    uint32_t old = get32(pad + 0x28 + (index + PAD_ENTRIES - 1) % PAD_ENTRIES * 16), changed = state ^ old;
    unsigned char* entry = pad + 0x28 + index * 16;
    put32(entry, state);
    put32(entry + 4, changed & state); /* delta_additions */
    put32(entry + 8, changed & old);   /* delta_removals */
    put16(entry + 12, (uint16_t)circle_x);
    put16(entry + 14, (uint16_t)circle_y);
    section_index(p, pad, index, now_ns);
    /* the touch screen in its pixels, 320 by 240 (the raw entry before that conversion is not implemented) */
    index = h->touch_index;
    h->touch_index = (index + 1) % TOUCH_ENTRIES;
    entry = touch + 0x20 + index * 8;
    put16(entry, input->touch ? (uint16_t)(input->touch_x * 320) : 0);
    put16(entry + 2, input->touch ? (uint16_t)(input->touch_y * 240) : 0);
    put32(entry + 4, input->touch);
    section_index(p, touch, index, now_ns);
    if (snx_ctr_phys_write(p->memory, physical + PAD, pad, sizeof pad) != SNX_CTR_MEM_OK ||
        snx_ctr_phys_write(p->memory, physical + TOUCH, touch, sizeof touch) != SNX_CTR_MEM_OK)
        return;
    signal_event(p, h->objects[1]);
    signal_event(p, h->objects[2]);
}
/* UpdateAccelerometerCallback and UpdateGyroscopeCallback: a reading of x, y and z, already scaled by the
   device's coefficient, and the raw entry made up from it as Azahar does */
static void motion(SnxCtrProcess* p, uint32_t section, uint32_t* next, uint32_t count, const int16_t value[3],
                   const int16_t raw[3], uint32_t event, uint64_t now_ns) {
    uint32_t physical = shared_physical(p);
    unsigned char b[0x20 + GYROSCOPE_COUNT * 6];
    size_t size = 0x20 + count * 6;
    if (!physical || snx_ctr_phys_read(p->memory, physical + section, b, size) != SNX_CTR_MEM_OK) return;
    uint32_t index = *next;
    *next = (index + 1) % count;
    for (unsigned i = 0; i < 3; ++i) {
        put16(b + 0x20 + index * 6 + i * 2, (uint16_t)value[i]);
        put16(b + 0x18 + i * 2, (uint16_t)raw[i]);
    }
    section_index(p, b, index, now_ns);
    if (snx_ctr_phys_write(p->memory, physical + section, b, size) == SNX_CTR_MEM_OK)
        signal_event(p, p->hid.objects[event]);
}
static void sample_accelerometer(SnxCtrProcess* p, uint64_t now_ns) {
    float accel[3] = {0, 0, 0}; /* the host has no motion sensor */
    int16_t value[3], raw[3];
    for (unsigned i = 0; i < 3; ++i) value[i] = (int16_t)(accel[i] * ACCELEROMETER_COEF);
    raw[0] = (int16_t)(-2 * value[0]);
    raw[1] = (int16_t)(-2 * value[2]);
    raw[2] = (int16_t)(2 * value[1]);
    motion(p, ACCELEROMETER, &p->hid.accelerometer_index, ACCELEROMETER_COUNT, value, raw, 3, now_ns);
}
static void sample_gyroscope(SnxCtrProcess* p, uint64_t now_ns) {
    float gyro[3] = {0, 0, 0}; /* the host has no motion sensor */
    int16_t value[3], raw[3];
    for (unsigned i = 0; i < 3; ++i) value[i] = (int16_t)(gyro[i] * GYROSCOPE_COEF);
    raw[0] = value[0];
    raw[1] = value[2];
    raw[2] = (int16_t)-value[1];
    motion(p, GYROSCOPE, &p->hid.gyroscope_index, GYROSCOPE_COUNT, value, raw, 4, now_ns);
}

/* the next deadline after one taken at now: a period on, or a period from now once too late to catch up */
static void advance(uint64_t* next, uint64_t period, uint64_t now_ns) {
    *next += period;
    if (now_ns > *next + MAX_LATE_NS) *next = now_ns + period;
}
void snx_ctr_hid_tick(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrHidService* h = &p->hid;
    if (!h->objects[0]) return;
    if (now_ns >= h->next_pad_ns) {
        SnxCtrInput input;
        snx_ctr_input_read(&input);
        snx_ctr_hid_pad(p, &input, now_ns);
        advance(&h->next_pad_ns, PAD_NS, now_ns);
    }
    if (h->accelerometer_enables > 0 && now_ns >= h->next_accelerometer_ns) {
        sample_accelerometer(p, now_ns);
        advance(&h->next_accelerometer_ns, ACCELEROMETER_NS, now_ns);
    }
    if (h->gyroscope_enables > 0 && now_ns >= h->next_gyroscope_ns) {
        sample_gyroscope(p, now_ns);
        advance(&h->next_gyroscope_ns, GYROSCOPE_NS, now_ns);
    }
}
uint64_t snx_ctr_hid_deadline(const SnxCtrProcess* p) {
    const SnxCtrHidService* h = &p->hid;
    if (!h->objects[0]) return UINT64_MAX;
    uint64_t deadline = h->next_pad_ns;
    if (h->accelerometer_enables > 0 && h->next_accelerometer_ns < deadline) deadline = h->next_accelerometer_ns;
    if (h->gyroscope_enables > 0 && h->next_gyroscope_ns < deadline) deadline = h->next_gyroscope_ns;
    return deadline;
}
static uint64_t host_now_ns(void) { return (uint64_t)(snx_seconds() * 1e9); }

static uint32_t snx_ctr_hid_GetIPCHandles(SnxCtrIpc* c, uint32_t* memory, uint32_t* pad1, uint32_t* pad2,
                                          uint32_t* accelerometer, uint32_t* gyroscope, uint32_t* debug_pad) {
    SnxCtrProcess* p = c->process;
    uint32_t* out[] = {memory, pad1, pad2, accelerometer, gyroscope, debug_pad};
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t result = p->hid.objects[0] ? snx_ctr_handle_add(p, p->hid.objects[i], out[i])
                                            : snx_ctr_object_create(p, i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED, out[i]);
        if (result) return result; /* generated output cleanup is transactional */
    }
    if (!p->hid.objects[0]) {
        uint32_t physical = snx_ctr_shared_pages(p, 4096);
        if (!physical) return 0xd86007f3u;
        for (unsigned i = 0; i < 6; ++i) {
            p->hid.objects[i] = snx_ctr_handle_object(p, *out[i]);
            ++p->objects[p->hid.objects[i]].refs;
        }
        /* One HLE system-pool page, outside the application pool/accounting. Board RAM is zero-initialized, and the
           pad and touch sampling starts from here (on hardware HID samples from boot, before any application) */
        CtrObject* shared = &p->objects[p->hid.objects[0]];
        shared->data.shared.physical = physical;
        shared->data.shared.size = 4096;
        shared->data.shared.permissions = SNX_CTR_READ;
        p->hid.next_pad_ns = host_now_ns() + PAD_NS;
    }
    return 0;
}
/* the accelerometer and the gyroscope are sampled while enabled, from a period after the first enable */
static void enable(int32_t* enables, uint64_t* next, uint64_t period) {
    if (++*enables == 1) *next = host_now_ns() + period;
}
static uint32_t snx_ctr_hid_EnableAccelerometer(SnxCtrIpc* c) {
    enable(&c->process->hid.accelerometer_enables, &c->process->hid.next_accelerometer_ns, ACCELEROMETER_NS);
    return 0;
}
static uint32_t snx_ctr_hid_DisableAccelerometer(SnxCtrIpc* c) {
    --c->process->hid.accelerometer_enables;
    return 0;
}
static uint32_t snx_ctr_hid_EnableGyroscopeLow(SnxCtrIpc* c) {
    enable(&c->process->hid.gyroscope_enables, &c->process->hid.next_gyroscope_ns, GYROSCOPE_NS);
    return 0;
}
static uint32_t snx_ctr_hid_DisableGyroscopeLow(SnxCtrIpc* c) {
    --c->process->hid.gyroscope_enables;
    return 0;
}
static uint32_t snx_ctr_hid_GetGyroscopeLowRawToDpsCoefficient(SnxCtrIpc* c, uint32_t* coefficient) {
    (void)c;
    float value = GYROSCOPE_COEF;
    memcpy(coefficient, &value, 4);
    return 0;
}
static uint32_t snx_ctr_hid_GetGyroscopeLowCalibrateParam(SnxCtrIpc* c, uint8_t* param) {
    (void)c;
    /* zero point, positive and negative unit points of x, y and z, then the reply's padding */
    memset(param, 0, 20);
    for (unsigned axis = 0; axis < 3; ++axis) {
        put16(param + axis * 6 + 2, GYROSCOPE_PARAM_UNIT);
        put16(param + axis * 6 + 4, (uint16_t)-GYROSCOPE_PARAM_UNIT);
    }
    return 0;
}
static uint32_t snx_ctr_hid_GetSoundVolume(SnxCtrIpc* c, uint32_t* volume) {
    (void)c;
    *volume = 0x3f; /* the volume slider, all the way up */
    return 0;
}
