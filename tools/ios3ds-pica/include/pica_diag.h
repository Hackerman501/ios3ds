/*
 * ios3ds-pica: staged GPU diagnostics for the first hardware runs (see
 * docs/pica200.md section 12). Run once at bring-up in thread context; each
 * step logs a "[gpu-diag]" line. Freestanding; the host test runs it against
 * the model, the shim against the real GPU.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include "pica_mirror.h"

#define PD_VRAM_PA    GM_TEX_PA                 /* the window the CPU sees: mirror buffers + diag areas */
#define PD_VRAM_SIZE  (0x18600000u - GM_TEX_PA)
#define PD_FC_SIZE    0x10000u                  /* FCRAM scratch the caller provides */

typedef struct {
    const pm_plat_t *p;
    volatile uint8_t *vram;     /* uncached CPU view of [PD_VRAM_PA, +PD_VRAM_SIZE) */
    uint8_t *fc_va;             /* PD_FC_SIZE of cached FCRAM (arena), 128-byte aligned */
    uint32_t fc_pa;
    uint8_t *blob_va;           /* gpumirror.bin, CPU view (the FCRAM test patches one word and restores it) */
    uint32_t blob_pa;
} pd_env_t;

enum {
    PD_FILL_VRAM = 1, PD_FILL_FCRAM = 2, PD_PPF = 4, PD_SOLID_VRAM = 8, PD_SOLID_FCRAM = 16,
    PD_TEX_POT = 32, PD_TEX_NPOT = 64,
};

void pd_dump_regs(const pd_env_t *e, const char *when);
uint32_t pd_fill_tests(const pd_env_t *e, const char *when);   /* PD_FILL_* */
uint32_t pd_gpu_tests(const pd_env_t *e);                      /* PD_PPF .. PD_TEX_NPOT */
/* words of the mirror texture / top target that hold what the self-test picture should give (of 16) */
uint32_t pd_dump_mirror(const pd_env_t *e);
