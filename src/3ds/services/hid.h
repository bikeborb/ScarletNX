/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_HID_SERVICE_H
#define SNX_CTR_HID_SERVICE_H
#include "3ds/input/input.h"
#include <stdint.h>
struct SnxCtrProcess;
/* One pad and touch sample of the host's input into the shared page, as HID's 234 Hz update writes it, then the two
   pad or touch events. No sample is written before the application's first GetIPCHandles made the page */
void snx_ctr_hid_pad(struct SnxCtrProcess* p, const SnxCtrInput* input, uint64_t now_ns);
/* The samples due by now (snx_ctr_schedule), and when the next one is: UINT64_MAX while there is no page */
void snx_ctr_hid_tick(struct SnxCtrProcess* p, uint64_t now_ns);
uint64_t snx_ctr_hid_deadline(const struct SnxCtrProcess* p);
#endif
