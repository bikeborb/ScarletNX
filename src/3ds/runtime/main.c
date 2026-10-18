/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"
#include "3ds/loader/ctr_loader.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/input/input.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR NULL
#endif

static void usage(FILE* out) {
    fputs("Usage: scarletnx-3ds [OPTIONS] game.cia\n"
          "       scarletnx-3ds [--model old|new] --board-info\n"
          "  game.cia        Extract, recompile if needed, then play a decrypted application CIA\n"
          "  -x, --extract   Extract only\n"
          "  -r, --recomp    Recompile without playing\n"
          "  --model old|new The console to model (default old)\n"
          "  --repo DIR      The data folder (default: the checkout, a portable copy's folder, else the\n"
          "                  user's data folder, as scarletnx picks it; an installed build's work/ is in\n"
          "                  its cache folder)\n"
          "  --output DIR    The extraction folder (default work/3ds/<the CIA's name>)\n"
          "  --max-blocks N  Stop after N native blocks (default: run until the game exits or its window\n"
          "                  closes)\n"
          "  --script S      Press buttons on a schedule, ms:hex,ms:hex,... as the Switch runtime's --script,\n"
          "                  each press of the HID buttons in hex held 300 ms (A 1, B 2, Select 4, Start 8,\n"
          "                  Right 10, Left 20, Up 40, Down 80, R 100, L 200, X 400, Y 800)\n"
          "Keys: A, S, Z, X for A, B, X, Y, T, G, F, H for the D-pad, Q, W for L, R, M, N for Start, Select,\n"
          "the arrows for the circle pad (D halves it), or the first gamepad.\n"
          "An extraction and a compiled module already there are used as they are: the ARM11 code is compiled\n"
          "again only when its module is missing or no longer matches the build or the extracted code.\n"
          "Encrypted content is not implemented.\n",
          out);
}
/* The extraction's folder name, from the CIA's file name in the Switch driver's alphabet */
static char* dump_name(const char* input) {
    const char *base = input, *end;
    for (const char* p = input; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    end = strrchr(base, '.');
    if (!end || end == base) end = base + strlen(base);
    char* name = malloc((size_t)(end - base) + 1);
    if (!name) return NULL;
    size_t n = 0;
    for (const char* p = base; p < end; ++p)
        if ((unsigned char)*p >= 128 || isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ' ')
            name[n++] = *p;
    while (n && name[n - 1] == ' ') --n;
    name[n] = 0;
    if (!n) {
        free(name);
        return snx_strdup("Game");
    }
    return name;
}
static bool cia_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'c' &&
           tolower((unsigned char)path[n - 2]) == 'i' && tolower((unsigned char)path[n - 1]) == 'a';
}
static bool visible_path(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
/* work/3ds of the data folder the Switch driver works in (snx_data_dir), which an installed build keeps in its cache
   folder (snx_work_dir); resolved, as the loader refuses a link on the way to its output */
static char* work_folder(const char* repo, const char* argv0) {
    char *exe = repo ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = repo ? snx_strdup(repo) : snx_data_dir(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    char *work = data ? snx_work_dir(data) : snx_strdup("work"), *folder = snx_path_join(work, "3ds");
    char* resolved = snx_absolute(folder);
    free(folder);
    free(work);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
int main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *output = NULL, *repo = NULL, *script = NULL;
    bool info = false, positional = false, extract_only = false, recompile = false, budget_set = false;
    uint64_t max_blocks = 0; /* no budget */
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && (!strcmp(arg, "-x") || !strcmp(arg, "--extract")))
            extract_only = true;
        else if (!positional && (!strcmp(arg, "-r") || !strcmp(arg, "--recomp")))
            recompile = true;
        else if (!positional && !strcmp(arg, "--output")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --output requires a directory\n", stderr);
                return 2;
            }
            output = argv[i];
        } else if (!positional && !strcmp(arg, "--repo")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --repo requires a directory\n", stderr);
                return 2;
            }
            repo = argv[i];
        } else if (!positional && !strcmp(arg, "--max-blocks")) {
            char* end = NULL;
            if (++i == argc || argv[i][0] < '0' || argv[i][0] > '9') {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                return 2;
            }
            errno = 0;
            unsigned long long n = strtoull(argv[i], &end, 10);
            if (errno || *end || !n) {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                return 2;
            }
            max_blocks = (uint64_t)n;
            budget_set = true;
        } else if (!positional && !strcmp(arg, "--script")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --script requires ms:hex,ms:hex,... with HID buttons in hex\n", stderr);
                return 2;
            }
            script = argv[i];
            if (!snx_ctr_input_script_valid(script)) {
                fputs("scarletnx-3ds: --script requires ms:hex,ms:hex,... with HID buttons in hex\n", stderr);
                return 2;
            }
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else
            input = arg;
    }
    if ((!input && !info) || (info && (input || extract_only || recompile || output || budget_set)) ||
        (extract_only && (budget_set || recompile || script)) || (recompile && (budget_set || script)) ||
        (info && script)) {
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) return 1;
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %" PRIu64 " Hz\nFCRAM: %u MiB\n", board.profile->name,
               board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ, board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    /* Do not let a host filename inject progress/launch banners into the GUI. */
    if (!visible_path(input) || (output && !visible_path(output))) {
        fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
        return 2;
    }
    if (!cia_path(input) || !snx_is_file(input)) {
        fputs("scarletnx-3ds: expected a readable .cia file\n", stderr);
        return 2;
    }
    char error[512], *extracted = NULL;
    if (output)
        extracted = snx_strdup(output); /* as given: the extractor refuses links on the way to it */
    else {
        char *work = work_folder(repo, argv[0]), *name = dump_name(input);
        extracted = work && name ? snx_path_join(work, name) : NULL;
        free(work);
        free(name);
    }
    if (!extracted) {
        fputs("scarletnx-3ds: cannot resolve the extraction folder\n", stderr);
        return 1;
    }
    /* like the Switch driver: a finished extraction is used again, the extractor publishes only complete ones */
    if (snx_is_dir(extracted))
        printf("Using the extraction in %s\n", extracted);
    else {
        printf("Extracting %s\n", input);
        char* published = NULL;
        SnxCtrExtractResult result = snx_ctr_extract(input, extracted, NULL, &published, error, sizeof error);
        free(extracted);
        if (result != SNX_CTR_EXTRACT_OK) {
            fprintf(stderr, "scarletnx-3ds: %s\n", error);
            return result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        }
        extracted = published;
        printf("Extracted %s\n", extracted);
    }
    if (extract_only) {
        free(extracted);
        return 0;
    }
    int status = snx_ctr_aot_run(extracted, model, max_blocks, recompile ? SNX_CTR_RECOMPILE | SNX_CTR_COMPILE_ONLY : 0,
                                 script, error, sizeof error);
    free(extracted);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
    return status;
}
