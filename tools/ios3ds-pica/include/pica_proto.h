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
#define PICA_PROTO_VERSION 2

enum {
    PICA_OP_INFO = 0,   /* -> a[] = pica_info_t (powers the GPU up and creates the arena on first use) */
    PICA_OP_ALLOC,      /* a0 size, a1 alignment (0 = default)        -> a0 pa */
    PICA_OP_FREE,       /* a0 pa */
    PICA_OP_WRITE,      /* a0 pa, a1 user source, a2 bytes */
    PICA_OP_READ,       /* a0 pa, a1 user destination, a2 bytes */
    PICA_OP_SUBMIT,     /* a0 user command list, a1 bytes (multiple of 16, ends in FINALIZE),
                           then optionally vertex data to copy first: a2 pa, a3 user source, a4 bytes */
    PICA_OP_FILL,       /* PSC memory fill, two slots: a0 pa, a1 bytes, a2 value, a3 PSC_FILL_16/24/32;
                           a4..a7 the same for slot 1 (bytes 0 = unused) */
    PICA_OP_TRANSFER,   /* PPF display transfer: a0 src pa, a1 src h<<16|w, a2 dst pa, a3 dst h<<16|w, a4 flags */
    PICA_OP_FREE_ALL,   /* free every block the calling process owns */
};

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
