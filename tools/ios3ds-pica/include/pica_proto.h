/*
 * ios3ds-pica: the contract between the kernel shim (kernel/shim/pica.c) and
 * userland (src/pica_dev_ios.c). Included by both: C only, fixed-width
 * types, no libc.
 *
 * Transport: the top-level sysctl node "ios3ds_gpu". A request is one extra
 * MIB component after the node: the address of a pica_req_t in the caller.
 *
 *     int mib[4]; size_t n = 4;
 *     sysctlnametomib("ios3ds_gpu", mib, &n);           // n == 1
 *     mib[n] = (int)&req;
 *     sysctl(mib, n + 1, NULL, NULL, NULL, 0);          // req.status = result
 *
 * (Not under "kern.": as "kern.ios3ds_gpu" the request call failed in QEMU.
 * Likely cause, from the xnu-1504 source: __sysctl sends every CTL_KERN name
 * through the old kern_sysctl() first, which answers ENOTDIR for a name with
 * more than one component after "kern" unless it is on a fixed list.)
 *
 * The shim copies the request in, runs it and copies it back; bulk data
 * (texels, vertices, command lists, read-back pixels) is copied between the
 * caller's buffers and the GPU arena by the shim as well. Nothing is written
 * through the sysctl interface itself, so it only needs sysctl *read* rights,
 * and userland never maps GPU registers or physical memory. Reading the node
 * without the extra component ("sysctl ios3ds_gpu") returns a
 * pica_info_t and has no side effects.
 *
 * The GPU works on physical addresses. All memory it touches is one
 * physically contiguous arena owned by the shim; `pa` values below are
 * physical addresses inside it, handed out by PICA_OP_ALLOC.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

#define PICA_SYSCTL_NAME   "ios3ds_gpu"
#define PICA_SYSCTL_LEAF   "ios3ds_gpu"
#define PICA_PROTO_VERSION 5

/* PRESENT flags bit 31: test hook (IOS3DS_GL_SCATTER=1 in the GL engine).
   The shim refuses FCRAM destinations with PICA_ERR_NOT_CONTIG so the
   PICA_OP_SCATTER path runs deterministically in QEMU; VRAM presents (the
   main display) are left alone. Never set by normal rendering. */
#define PICA_PRESENT_TEST_SCATTER (1u << 31)

enum {
    PICA_OP_INFO = 0,   /* -> a[] = pica_info_t (powers the GPU up and creates the arena on first use) */
    PICA_OP_ALLOC,      /* a0 size, a1 alignment (0 = default)        -> a0 pa */
    PICA_OP_FREE,       /* a0 pa */
    PICA_OP_WRITE,      /* a0 pa, a1 user source, a2 bytes */
    PICA_OP_READ,       /* a0 pa, a1 user destination, a2 bytes */
    PICA_OP_SUBMIT,     /* a0 user command list, a1 bytes (multiple of 16, ends in FINALIZE),
                           then optionally vertex data to copy first: a2 pa, a3 user source, a4 bytes */
    PICA_OP_FILL,       /* constant fill of arena memory, two slots: a0 pa, a1 bytes, a2 value,
                           a3 PSC_FILL_16/24/32; a4..a7 the same for slot 1 (bytes 0 = unused).
                           Hardware runs 12/13: PSC fills do not write FCRAM, so the shim's
                           driver does this one on the CPU (a constant is tiled-invariant) */
    PICA_OP_TRANSFER,   /* PPF display transfer: a0 src pa, a1 src h<<16|w, a2 dst pa, a3 dst h<<16|w, a4 flags */
    PICA_OP_FREE_ALL,   /* free every block the calling process owns */
    PICA_OP_CONFIG,     /* -> a0 PICA_CFG_* flags from boot.cfg (does not bring the GPU up) */
    PICA_OP_PRESENT,    /* PPF display transfer from the arena straight into the caller's own memory:
                           a0 src pa, a1 src h<<16|w, a2 user destination, a3 bytes, a4 flags (out linear).
                           The destination must be user-writable in the caller, physically contiguous,
                           in FCRAM or VRAM; else PICA_ERR_NOT_CONTIG / PICA_ERR_FAULT */
    PICA_OP_SCATTER,    /* copy an arena block into the caller's own memory page by page, for a
                           destination that is not physically contiguous (PICA_OP_PRESENT refused
                           it): a0 src pa, a1 user destination (page aligned), a2 bytes. The shim
                           walks the caller's page tables and copies each page through the kernel's
                           linear map with the caller's cache lines maintained. Every page must be
                           user-writable and in FCRAM or VRAM; else PICA_ERR_FAULT / PICA_ERR_NOT_CONTIG */
    PICA_OP_DIRTY,      /* a surface target was written back by the CPU after a PICA_OP_READ.
                           The display mirror's damage tracking needs to know: PRESENT and
                           SCATTER already mark the frame dirty, but a CPU read-back (the
                           stereo stamp path, or the fallback after a refused present) writes
                           straight into the IOSurface from userland. No arguments. */
};

#define PICA_CFG_CA_GPU 1u      /* boot.cfg gpu=1 and ca_gpu=1: CoreAnimation may composite on the GPU */

enum {
    PICA_OK = 0,
    PICA_ERR_BAD_OP = 1,
    PICA_ERR_GPU_OFF = 2,   /* PDN_GPU_CNT: the GPU is held in reset / unclocked and could not be started */
    PICA_ERR_NO_ARENA = 3,  /* no physically contiguous memory for the arena */
    PICA_ERR_NO_MEM = 4,    /* arena full (or block table full) */
    PICA_ERR_RANGE = 5,     /* address / size outside the arena or the caller's block */
    PICA_ERR_FAULT = 6,     /* bad user pointer */
    PICA_ERR_TIMEOUT = 7,   /* the GPU did not finish in time; details are in the console log */
    PICA_ERR_NO_SERVICE = 8,/* userland only: the sysctl does not exist (no shim support) */
    PICA_ERR_NOT_CONTIG = 9,/* PRESENT: the destination is not one physically contiguous GPU-reachable range */
};

typedef struct {
    uint32_t op;
    uint32_t status;        /* out: PICA_OK or PICA_ERR_* */
    uint32_t a[8];
} pica_req_t;

typedef struct {
    uint32_t version;       /* PICA_PROTO_VERSION */
    uint32_t state;         /* PICA_OK once the GPU and arena are up, else why not */
    uint32_t gpu_cnt;       /* PDN_GPU_CNT as found at first use */
    uint32_t arena_pa;
    uint32_t arena_size;
    uint32_t arena_free;    /* bytes */
    uint32_t submits;       /* command lists run since boot */
    uint32_t timeouts;      /* GPU operations that timed out since boot */
} pica_info_t;

_Static_assert(sizeof(pica_info_t) == 8 * 4, "pica_info_t fits pica_req_t.a");
