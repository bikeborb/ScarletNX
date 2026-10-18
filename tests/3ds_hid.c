/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   hid:USER's samples in the shared page, where libctru's hidScanInput reads them, and its events and commands. */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/hid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CMD (SNX_CTR_INITIAL_TLS + 0x80)
static void put(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(value >> (8 * i));
    assert(snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, unsigned word) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, CMD + word * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void call(SnxCtrProcess* p, uint32_t handle, uint32_t header) {
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, CMD, zero, sizeof zero) == SNX_CTR_MEM_OK);
    put(p, CMD, header);
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!get(p, 1));
}
/* a word of the shared page */
static uint32_t shared(SnxCtrProcess* p, uint32_t offset) {
    unsigned char b[4];
    uint32_t physical = p->objects[p->hid.objects[0]].data.shared.physical;
    assert(snx_ctr_phys_read(p->memory, physical + offset, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static bool signaled(SnxCtrProcess* p, unsigned event) { return p->objects[p->hid.objects[event]].data.event.signaled; }
static void clear(SnxCtrProcess* p) {
    for (unsigned i = 1; i < 6; ++i) p->objects[p->hid.objects[i]].data.event.signaled = 0;
}

int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 4096,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    SnxCtrInput input = {.buttons = SNX_CTR_PAD_A, .circle_x = 1};
    /* nothing is sampled before the page exists */
    snx_ctr_hid_pad(p, &input, 1000000000u);
    snx_ctr_hid_tick(p, UINT64_MAX);
    assert(snx_ctr_hid_deadline(p) == UINT64_MAX);
    uint32_t hid;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &hid));
    call(p, hid, 0xa0000);
    assert(get(p, 0) == 0xa0047);
    uint32_t pad1 = get(p, 4);
    /* the first pad sample is due one 234 Hz period on */
    uint64_t deadline = snx_ctr_hid_deadline(p);
    assert(deadline != UINT64_MAX && deadline == p->hid.next_pad_ns);
    /* a thread waiting on the first pad event wakes at the sample */
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = pad1;
    r[2] = r[3] = UINT32_MAX;
    assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_thread(p)->waiting_object == p->hid.objects[1]);
    uint64_t now = (uint64_t)((p->clock_origin + 2.0) * 1e9);
    snx_ctr_hid_pad(p, &input, now);
    assert(!snx_ctr_thread(p)->waiting_object && !signaled(p, 1) && signaled(p, 2)); /* one-shot: taken by the wake */
    /* entry 0: A, and the circle pad all the way right averaged with two zero samples, 0x9a / 3, past the threshold */
    assert(shared(p, 0x10) == 0 && shared(p, 0x1c) == (SNX_CTR_PAD_A | 1u << 28));
    assert(shared(p, 0x28) == (SNX_CTR_PAD_A | 1u << 28) && shared(p, 0x2c) == shared(p, 0x28) && !shared(p, 0x30));
    assert(shared(p, 0x34) == 51);
    uint64_t ticks = shared(p, 0) | (uint64_t)shared(p, 4) << 32;
    assert(ticks >= 2 * 268111856u - 2 && ticks <= 2 * 268111856u + 2 && !shared(p, 8) && !shared(p, 12));
    /* libctru's touch section: entry 0 written, not pressed, with its own timestamp */
    assert(shared(p, 0xa8 + 0x10) == 0 && shared(p, 0xa8) == shared(p, 0) && !shared(p, 0xa8 + 0x24));
    /* entry 1: A released, the circle pad still averaged right, at (154 + 154 + 0) / 3 */
    clear(p);
    input = (SnxCtrInput){.circle_x = 1, .touch = true, .touch_x = 0.5f, .touch_y = 0.25f};
    snx_ctr_hid_pad(p, &input, now + 5000000u);
    assert(signaled(p, 1) && signaled(p, 2));
    assert(shared(p, 0x10) == 1 && shared(p, 0x38) == 1u << 28 && !shared(p, 0x3c) && shared(p, 0x40) == SNX_CTR_PAD_A);
    assert(shared(p, 0x44) == 102);
    assert(shared(p, 0xa8 + 0x10) == 1 && shared(p, 0xa8 + 0x28) == (160u | 60u << 16) && shared(p, 0xa8 + 0x2c) == 1);
    /* the circle pad up and left at the threshold's angles: 30 degrees off the x axis is only left */
    input = (SnxCtrInput){.circle_x = -0.9f, .circle_y = 0.4f};
    for (unsigned i = 0; i < 3; ++i) snx_ctr_hid_pad(p, &input, now + 10000000u);
    assert(shared(p, 0x10) == 4 && shared(p, 0x28 + 4 * 16) == 1u << 29);
    input.circle_y = 0.9f;
    for (unsigned i = 0; i < 3; ++i) snx_ctr_hid_pad(p, &input, now + 10000000u);
    assert(shared(p, 0x10) == 7 && shared(p, 0x28 + 7 * 16) == (1u << 29 | 1u << 30));
    /* the ninth sample wraps to entry 0, and the timestamp before it moves to index_reset_ticks_previous */
    snx_ctr_hid_pad(p, &input, now + 1000000000u);
    assert(shared(p, 0x10) == 0 && (shared(p, 8) | (uint64_t)shared(p, 12) << 32) == ticks);
    assert((shared(p, 0) | (uint64_t)shared(p, 4) << 32) > ticks);
    /* a late tick samples once and starts again a period from then, no burst of catching up */
    uint64_t late = p->hid.next_pad_ns + 1000000000u;
    snx_ctr_hid_tick(p, late);
    assert(shared(p, 0x10) == 1 && p->hid.next_pad_ns == late + 4273503u); /* BASE_CLOCK / 234 ticks */
    /* the accelerometer is sampled only while enabled, and signals its own event */
    uint64_t pad_deadline = snx_ctr_hid_deadline(p);
    call(p, hid, 0x110000);
    assert(get(p, 0) == 0x110040 && p->hid.accelerometer_enables == 1);
    assert(snx_ctr_hid_deadline(p) ==
           (pad_deadline < p->hid.next_accelerometer_ns ? pad_deadline : p->hid.next_accelerometer_ns));
    clear(p);
    snx_ctr_hid_tick(p, p->hid.next_accelerometer_ns);
    assert(signaled(p, 3) && !signaled(p, 4) && shared(p, 0x108 + 0x10) == 0 && shared(p, 0x108));
    call(p, hid, 0x120000);
    assert(get(p, 0) == 0x120040 && !p->hid.accelerometer_enables);
    clear(p);
    snx_ctr_hid_tick(p, UINT64_MAX - 1000000000u);
    assert(!signaled(p, 3));
    call(p, hid, 0x130000);
    clear(p);
    snx_ctr_hid_tick(p, p->hid.next_gyroscope_ns);
    assert(signaled(p, 4) && shared(p, 0x158));
    call(p, hid, 0x140000);
    assert(!p->hid.gyroscope_enables);
    /* the gyroscope's coefficient and calibration, and the volume slider */
    call(p, hid, 0x150000);
    assert(get(p, 0) == 0x150080 && get(p, 2) == 0x41660000u); /* 14.375f */
    call(p, hid, 0x160000);
    assert(get(p, 0) == 0x160180);
    for (unsigned axis = 0; axis < 3; ++axis) {
        unsigned char b[6];
        assert(snx_ctr_read_ram(p->space, CMD + 8 + axis * 6, b, 6) == SNX_CTR_MEM_OK);
        assert(!b[0] && !b[1] && (b[2] | b[3] << 8) == 6700 && (b[4] | b[5] << 8) == (uint16_t)-6700);
    }
    call(p, hid, 0x170000);
    assert(get(p, 0) == 0x170080 && get(p, 2) == 0x3f);
    snx_ctr_process_free(p);
    puts("HID pad, circle pad, touch and motion samples, their events and the hid:USER commands passed");
    return 0;
}
