/*
 * ios3ds-pica: the GPU display mirror engine (see boot/common/gpu_mirror.h
 * and docs/pica200.md section 9). Freestanding like pica_drv.c: the same
 * file runs in the kernel shim (real GX registers, stepped from the VBlank
 * and timer interrupts) and on the build host against the software model
 * (tools/ios3ds-pica/tests/test_mirror.c).
 *
 * A frame is a short queue of GPU operations (PPF upload, P3D list, PPF
 * read-outs). Nothing here waits unless asked to: pm_begin() starts the
 * first operation, pm_poll() starts the next one whenever the current one
 * has finished, so the caller can step it from interrupts.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include "gpu_mirror.h"

typedef struct pm_plat {
    uint32_t (*rd)(uint32_t off);           /* GX register, byte offset from 0x10400000 */
    void (*wr)(uint32_t off, uint32_t v);
    uint32_t (*ticks)(void);                /* free-running up-counter */
    uint32_t tick_hz;
    void (*to_gpu)(void *p, uint32_t len);  /* self-test buffers: clean / invalidate */
    void (*from_gpu)(void *p, uint32_t len);
    void (*log)(const char *fmt, ...);      /* thread context only (self-test) */
    void (*relax)(void);                    /* thread context: pause between polls (may be NULL) */
} pm_plat_t;

typedef struct {
    uint32_t src_pa;            /* linear BGRA 320x480, stride 1280 */
    uint32_t mode;              /* 0 top, 1 both, 2 bottom (display.h SCREEN_*) */
    uint32_t nsets;             /* 1 or 2 framebuffer sets */
    uint32_t top[2], bot[2];    /* BGR8 LCD framebuffers (PA) */
} pm_frame_t;

enum { PM_IDLE = 0, PM_BUSY = 1, PM_DONE = 2, PM_TIMEOUT = 3 };
enum { PM_OP_PPF = 0, PM_OP_P3D = 1, PM_OP_TEXCOPY = 2 };
#define PM_MAX_OPS 8

typedef struct pica_mirror {
    const pm_plat_t *p;
    uint32_t list_pa[2], list_bytes[2];     /* from gpumirror.bin */
    uint32_t tex_pa;                        /* the 512x512 texture (FCRAM) */
    uint32_t variant;                       /* GM_VAR_* */
    uint32_t timeout_ticks;
    /* the frame in flight */
    struct { uint8_t kind, stage; uint32_t a, b, c, d, e; } op[PM_MAX_OPS];
    uint32_t nops, cur, state;
    uint32_t t_frame0, t_op0, t_stage0[3];
    /* statistics (ticks) */
    uint32_t t_stage[3], m_stage[3], t_frame, m_frame, sum_frame;
    uint32_t frames, timeouts;
    uint32_t why_timeout;                   /* GX register snapshot of the last timeout */
} pica_mirror_t;

/* Point the engine at a loaded gpumirror.bin (blob = its CPU view) and at
   its texture (GM_TEX_BYTES, 8-byte aligned, GPU-reachable; the shim takes it
   from the FCRAM arena): the two lists get the texture address patched in.
   Returns 0 when the blob is valid for this build's layout. */
int pm_init(pica_mirror_t *m, const pm_plat_t *p, void *blob, uint32_t blob_pa, uint32_t tex_pa);

/* Black-fill both render targets (their margins are never drawn). Waits. */
int pm_clear_targets(pica_mirror_t *m);

/* Start a frame (the GPU must be idle and owned by the caller). */
void pm_begin(pica_mirror_t *m, const pm_frame_t *f);
/* Advance: start the next operation of a finished one; spin at most
   spin_ticks for the current one. PM_BUSY, PM_DONE (frame complete) or
   PM_TIMEOUT (the frame was abandoned). */
int pm_poll(pica_mirror_t *m, uint32_t spin_ticks);

/*
 * Self-test (thread context): mirror a synthetic 320x480 picture written to
 * src (CPU view src_va, GPU view src_pa, cached memory) into out (top
 * framebuffer image followed directly by the bottom one, GM_TOP_FB_BYTES +
 * GM_BOT_FB_BYTES, with `guard` bytes of sentinel before and after) and
 * compare every byte with gm_map(). Tries the GM_VAR_* variants in order
 * and keeps the first exact one in m->variant.
 * Returns the variant, or PM_SELFTEST_MISMATCH / PM_SELFTEST_TIMEOUT. bad0 / first_bad[4] describe the default
 * variant's result (mismatching bytes; offset, expected, got, ops).
 */
int pm_selftest(pica_mirror_t *m, uint8_t *src_va, uint32_t src_pa, uint8_t *out_va, uint32_t out_pa,
                uint32_t guard, uint32_t *bad0, uint32_t first_bad[4]);
#define PM_SELFTEST_MISMATCH (-1)   /* no variant matched (or the guard was hit) */
#define PM_SELFTEST_TIMEOUT  (-2)   /* a GPU operation did not finish: first_bad[0] = operation, [3] = register */

/* The self-test picture: unique per pixel, alpha deliberately not 0xFF. */
static inline uint32_t pm_pattern(uint32_t x, uint32_t y)
{
    uint32_t b = x & 0xFF, g = y & 0xFF, r = ((x >> 8) | (y >> 8) << 2 | ((x ^ y) & 7) << 5) ^ 0x15;
    uint32_t a = (x * 7 + y * 13) & 0xFF;
    return a << 24 | r << 16 | g << 8 | b;   /* bytes in memory: B, G, R, A */
}
