/*
 * ios3ds-pica: how a GL back end reaches the GPU. One table of operations,
 * two implementations:
 *   pica_dev_ios()   the ios3ds_gpu sysctl -> kernel shim  (src/pica_dev_ios.c)
 *   pica_dev_host()  driver core + software PICA model, in-process (src/pica_dev_host.c)
 * Every call returns PICA_OK or a PICA_ERR_* code (pica_proto.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include "pica_proto.h"

typedef struct pica_dev {
    const char *name;
    uint32_t (*info)(pica_info_t *out);
    uint32_t (*alloc)(uint32_t size, uint32_t align, uint32_t *pa);
    uint32_t (*free)(uint32_t pa);
    uint32_t (*write)(uint32_t pa, const void *src, uint32_t len);
    uint32_t (*read)(uint32_t pa, void *dst, uint32_t len);
    /* run a finished command list; vtx (may be NULL) is copied to vtx_pa first */
    uint32_t (*submit)(const uint32_t *list, uint32_t bytes, uint32_t vtx_pa, const void *vtx, uint32_t vtx_len);
    uint32_t (*fill)(uint32_t pa0, uint32_t len0, uint32_t val0, uint32_t width0,
                     uint32_t pa1, uint32_t len1, uint32_t val1, uint32_t width1);
    uint32_t (*transfer)(uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa, uint32_t dst_dim, uint32_t flags);
    uint32_t (*config)(uint32_t *flags);            /* PICA_CFG_* */
} pica_dev_t;

const pica_dev_t *pica_dev_ios(void);
const pica_dev_t *pica_dev_host(void);
const char *pica_strerror(uint32_t status);

/* host only: the model's counters (struct PicaSimStats) for tests */
const void *pica_dev_host_stats(void);
