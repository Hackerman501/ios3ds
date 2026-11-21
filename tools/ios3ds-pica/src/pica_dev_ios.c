/*
 * ios3ds-pica: the GPU as iOS userland sees it: requests to the kernel shim
 * through the ios3ds_gpu sysctl (include/pica_proto.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <string.h>
#include "pica_dev.h"

extern int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern int sysctlnametomib(const char *name, int *mibp, size_t *sizep);

extern int *__error(void);
extern int snprintf(char *, size_t, const char *, ...);

static int mib[4];
static size_t miblen;
static char why[96] = "no " PICA_SYSCTL_NAME " sysctl service";

static uint32_t call(pica_req_t *r)
{
    if (!miblen) {
        size_t n = 3;
        if (sysctlnametomib(PICA_SYSCTL_NAME, mib, &n) != 0 || n < 1 || n > 3) {
            snprintf(why, sizeof why, "no sysctl node \"" PICA_SYSCTL_NAME "\" (errno %d): shim without GPU support",
                     *__error());
            return PICA_ERR_NO_SERVICE;
        }
        miblen = n;
    }
    r->status = PICA_ERR_BAD_OP;
    mib[miblen] = (int)(uintptr_t)r;
    if (sysctl(mib, (unsigned)miblen + 1, NULL, NULL, NULL, 0) != 0) {
        snprintf(why, sizeof why, "sysctl " PICA_SYSCTL_NAME " request %u failed (errno %d)", r->op, *__error());
        return PICA_ERR_NO_SERVICE;
    }
    return r->status;
}

static uint32_t d_info(pica_info_t *out)
{
    pica_req_t r = { PICA_OP_INFO, 0, { 0 } };
    uint32_t st = call(&r);
    memcpy(out, r.a, sizeof *out);
    return st;
}
static uint32_t d_alloc(uint32_t size, uint32_t align, uint32_t *pa)
{
    pica_req_t r = { PICA_OP_ALLOC, 0, { size, align } };
    uint32_t st = call(&r);
    *pa = r.a[0];
    return st;
}
static uint32_t d_free(uint32_t pa)
{
    pica_req_t r = { PICA_OP_FREE, 0, { pa } };
    return call(&r);
}
static uint32_t d_write(uint32_t pa, const void *src, uint32_t len)
{
    pica_req_t r = { PICA_OP_WRITE, 0, { pa, (uint32_t)(uintptr_t)src, len } };
    return call(&r);
}
static uint32_t d_read(uint32_t pa, void *dst, uint32_t len)
{
    pica_req_t r = { PICA_OP_READ, 0, { pa, (uint32_t)(uintptr_t)dst, len } };
    return call(&r);
}
static uint32_t d_submit(const uint32_t *list, uint32_t bytes, uint32_t vtx_pa, const void *vtx, uint32_t vtx_len)
{
    pica_req_t r = { PICA_OP_SUBMIT, 0, { (uint32_t)(uintptr_t)list, bytes, vtx_pa, (uint32_t)(uintptr_t)vtx, vtx_len } };
    return call(&r);
}
static uint32_t d_fill(uint32_t pa0, uint32_t len0, uint32_t val0, uint32_t width0,
                       uint32_t pa1, uint32_t len1, uint32_t val1, uint32_t width1)
{
    pica_req_t r = { PICA_OP_FILL, 0, { pa0, len0, val0, width0, pa1, len1, val1, width1 } };
    return call(&r);
}
static uint32_t d_transfer(uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa, uint32_t dst_dim, uint32_t flags)
{
    pica_req_t r = { PICA_OP_TRANSFER, 0, { src_pa, src_dim, dst_pa, dst_dim, flags } };
    return call(&r);
}

static uint32_t d_config(uint32_t *flags)
{
    pica_req_t r = { PICA_OP_CONFIG, 0, { 0 } };
    uint32_t st = call(&r);
    *flags = st == PICA_OK ? r.a[0] : 0;
    return st;
}

static uint32_t d_present(uint32_t src_pa, uint32_t dim, void *dst, uint32_t bytes, uint32_t flags)
{
    pica_req_t r = { PICA_OP_PRESENT, 0, { src_pa, dim, (uint32_t)(uintptr_t)dst, bytes, flags } };
    return call(&r);
}

static uint32_t d_scatter(uint32_t src_pa, void *dst, uint32_t bytes)
{
    pica_req_t r = { PICA_OP_SCATTER, 0, { src_pa, (uint32_t)(uintptr_t)dst, bytes } };
    return call(&r);
}

const pica_dev_t *pica_dev_ios(void)
{
    static const pica_dev_t dev = { "PICA200", d_info, d_alloc, d_free, d_write, d_read, d_submit, d_fill, d_transfer,
                                    d_config, d_present, d_scatter };
    return &dev;
}

const char *pica_strerror(uint32_t st)
{
    static const char *const s[] = { "ok", "bad request", "GPU is powered off / in reset", "no contiguous memory for the arena",
        "GPU arena full", "address outside the arena", "bad user pointer", "GPU timeout (see console log)", why,
        "destination is not one physically contiguous, GPU-reachable, user-writable range" };
    return st < sizeof s / sizeof s[0] ? s[st] : "unknown error";
}
