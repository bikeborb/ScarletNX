/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_compile.h"
#include "common/utilities/common.h"
#include "3ds/runtime/internal.h"
#include "3ds/gpu/present.h"
#include "3ds/audio/output.h"
#include "3ds/input/input.h"
#include "common/filesystem/host.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define SNX_CTR_RUN_SLICE 1000000u

int snx_ctr_aot_run(const char* directory, SnxCtrModel model, uint64_t limit, unsigned flags, const char* script,
                    char* error, size_t error_size) {
    SnxCtrImage image = {0};
    SnxCtrProcess* process = NULL;
    SnxCtrNative native = {0};
    char *module_path = NULL, *frames = NULL, *wav = NULL;
    int result = 1;
    if (!snx_ctr_image_load(directory, &image, error, error_size)) goto done;
    module_path = snx_format("%s/native/code.so", directory);
    /* like the Switch driver's recompile: only when needed, an existing module is no error */
    if ((flags & SNX_CTR_RECOMPILE) || !snx_ctr_native_open(module_path, &image, &native, error, error_size)) {
        snx_ctr_native_close(&native);
        printf("Recompiling ARM11 (offline static AOT)\n");
        int status = snx_ctr_aot_compile(directory, error, error_size);
        if (status) {
            result = status == 3 ? 3 : 1;
            goto done;
        }
        if (!snx_ctr_native_open(module_path, &image, &native, error, error_size)) goto done;
    }
    if (flags & SNX_CTR_COMPILE_ONLY) {
        result = 0;
        goto done;
    }
    process = snx_ctr_process_new(&image, model);
    if (!process) {
        snprintf(error, error_size, "cannot create checked ARM11 process mappings");
        goto done;
    }
    if (!snx_ctr_process_mount_fs(process, directory)) {
        snprintf(error, error_size, "cannot index extracted title filesystem");
        goto done;
    }
    /* Vulkan-only presentation: real frames land in <output>/frames as PPM.
       Without a Vulkan loader/device no frame is shown or fabricated. */
    frames = snx_path_join(directory, "frames");
    if (!frames || (!snx_is_dir(frames) && plat_mkdir(frames))) {
        snprintf(error, error_size, "cannot create frame output directory");
        goto done;
    }
    if (!snx_ctr_present_init(frames)) printf("Vulkan presenter unavailable: no frames will be shown or saved\n");
    /* the DSP's output plays on the default device, and its first two minutes go to <output>/audio.wav */
    wav = snx_path_join(directory, "audio.wav");
    if (!snx_ctr_audio_init(wav)) printf("No audio output: the DSP's frames will not be heard or saved\n");
    if (!snx_ctr_input_init(script)) {
        snprintf(error, error_size, "cannot keep the --script presses"); /* out of memory, main checked it */
        goto done;
    }
    /* Runs until the game exits or its window is closed, as a Switch game does, or for the --max-blocks budget. The
       dispatch goes in slices so the window's events are seen between them */
    uint64_t executed = 0;
    bool closed = false;
    SnxCtrRunResult state;
    printf("Running ARM11 static AOT\n");
    for (;;) {
        uint64_t slice = SNX_CTR_RUN_SLICE, n = 0;
        if (limit && limit - executed < slice) slice = limit - executed;
        state = snx_ctr_dispatch(native.module, process, slice, &n);
        executed += n;
        if (state != SNX_CTR_RUN_BUDGET || (limit && executed >= limit)) break;
        if ((closed = snx_ctr_present_closed())) break;
    }
    SnxArmCPU* cpu = snx_ctr_process_cpu(process);
    if (state == SNX_CTR_RUN_EXIT) {
        printf("Guest ExitProcess: r0=%" PRIu32 " blocks=%" PRIu64 "\n", cpu->r[0], executed);
        result = 0;
    } else if (closed) {
        printf("Closed: blocks=%" PRIu64 "\n", executed);
        result = 0;
    } else {
        snprintf(error, error_size,
                 "%s: pc=0x%08" PRIx32 " exit=%u syndrome=0x%08" PRIx32 " blocks=%" PRIu64 " svcs=%" PRIu64
                 " last_svc=0x%02" PRIx32 " ipc_service=%" PRIu32 " ipc_header=0x%08" PRIx32 " wait_service=%s",
                 state == SNX_CTR_RUN_BLOCKED       ? "guest blocked (no runnable thread)"
                 : state == SNX_CTR_RUN_BUDGET      ? "static block budget exhausted"
                 : state == SNX_CTR_RUN_UNSUPPORTED ? "unsupported guest operation or missing static entry"
                                                    : "guest fault",
                 cpu->r[15], cpu->exit, cpu->syndrome, executed, process->svc_count, process->last_svc,
                 process->last_ipc_service, process->last_ipc_header, snx_ctr_thread(process)->waiting_service);
        result = state == SNX_CTR_RUN_FAULT ? 1 : 3;
    }
done:
    snx_ctr_present_free();
    snx_ctr_input_free();
    snx_ctr_audio_free();
    snx_ctr_process_free(process);
    free(wav);
    free(frames);
    snx_ctr_native_close(&native);
    free(module_path);
    snx_ctr_image_free(&image);
    return result;
}
