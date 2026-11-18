/*
 * ios3ds-pica: host "GPU": the driver core talking to the software PICA
 * model over its MMIO interface, with a malloc'ed arena standing in for
 * physical memory. Used by the tests and by the GL engine's host build.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pica_dev.h"
#include "pica_drv.h"
#include "../sim/picasim.h"

#define HOST_ARENA_PA   0x26000000u
#define HOST_ARENA_SIZE 0x01000000u

static PicaSim sim;
static pica_drv_t drv;
static uint8_t *arena;

static uint8_t *h_mem(void *o, uint32_t pa, uint32_t len, int write)
{
    (void)o; (void)write;
    if (pa < HOST_ARENA_PA || len > HOST_ARENA_SIZE || pa - HOST_ARENA_PA > HOST_ARENA_SIZE - len)
        return NULL;
    return arena + (pa - HOST_ARENA_PA);
}
static void h_log(void *o, const char *msg) { (void)o; fprintf(stderr, "%s\n", msg); }

static uint32_t p_rd(uint32_t off) { return picasim_read(&sim, off); }
static void p_wr(uint32_t off, uint32_t v) { picasim_write(&sim, off, v); }
static int p_copyin(pica_uptr_t src, void *dst, uint32_t len) { memcpy(dst, (const void *)src, len); return 0; }
static int p_copyout(const void *src, pica_uptr_t dst, uint32_t len) { memcpy((void *)dst, src, len); return 0; }
static void p_sync(void *p, uint32_t len) { (void)p; (void)len; }
static void p_udelay(uint32_t us) { (void)us; }
static int p_alive(uint32_t owner) { (void)owner; return 1; }
static void p_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static const pica_plat_t plat = { p_rd, p_wr, p_copyin, p_copyout, p_sync, p_sync, p_udelay, p_alive, p_log };

static void up(void)
{
    static const PicaSimHost host = { h_mem, NULL, h_log, NULL };
    if (arena)
        return;
    arena = malloc(HOST_ARENA_SIZE);
    memset(arena, 0xA5, HOST_ARENA_SIZE);           /* nothing may rely on zeroed memory */
    picasim_init(&sim, &host);
    pica_drv_init(&drv, &plat, arena, HOST_ARENA_PA, HOST_ARENA_SIZE, 0x1007F);
}

static uint32_t d_info(pica_info_t *out) { up(); pica_drv_info(&drv, out); return PICA_OK; }
static uint32_t d_alloc(uint32_t size, uint32_t align, uint32_t *pa) { up(); return pica_drv_alloc(&drv, 1, size, align, pa); }
static uint32_t d_free(uint32_t pa) { return pica_drv_free(&drv, 1, pa); }
static uint32_t d_write(uint32_t pa, const void *src, uint32_t len) { return pica_drv_write(&drv, pa, (pica_uptr_t)src, len); }
static uint32_t d_read(uint32_t pa, void *dst, uint32_t len) { return pica_drv_read(&drv, pa, (pica_uptr_t)dst, len); }
static uint32_t d_submit(const uint32_t *list, uint32_t bytes, uint32_t vtx_pa, const void *vtx, uint32_t vtx_len)
{
    return pica_drv_submit(&drv, (pica_uptr_t)list, bytes, vtx_pa, (pica_uptr_t)vtx, vtx_len);
}
static uint32_t d_fill(uint32_t pa0, uint32_t len0, uint32_t val0, uint32_t width0,
                       uint32_t pa1, uint32_t len1, uint32_t val1, uint32_t width1)
{
    uint32_t a[8] = { pa0, len0, val0, width0, pa1, len1, val1, width1 };
    return pica_drv_fill(&drv, a);
}
static uint32_t d_transfer(uint32_t src_pa, uint32_t src_dim, uint32_t dst_pa, uint32_t dst_dim, uint32_t flags)
{
    return pica_drv_transfer(&drv, src_pa, src_dim, dst_pa, dst_dim, flags);
}

static uint32_t d_config(uint32_t *flags)
{
    const char *e = getenv("IOS3DS_HOST_CA_GPU");           /* tests: what boot.cfg would say */
    *flags = e && *e == '0' ? 0 : PICA_CFG_CA_GPU;
    return PICA_OK;
}

/* the host has no physical addresses for the caller's memory: transfer into
   a scratch block of the arena and copy it out, which is what the GPU's
   transfer does on iOS (IOS3DS_HOST_PRESENT=0: behave like a destination
   that is not contiguous) */
static uint32_t d_present(uint32_t src_pa, uint32_t dim, void *dst, uint32_t bytes, uint32_t flags)
{
    const char *e = getenv("IOS3DS_HOST_PRESENT");
    uint32_t pa, st;
    if (e && *e == '0')                         /* IOS3DS_HOST_PRESENT=0: refuse, =swap: R and B swapped */
        return PICA_ERR_NOT_CONTIG;
    if ((st = pica_drv_alloc(&drv, 2, bytes, 0, &pa)) != PICA_OK)
        return st;
    st = pica_drv_transfer(&drv, src_pa, dim, pa, dim, flags);
    if (st == PICA_OK)
        memcpy(dst, arena + (pa - HOST_ARENA_PA), bytes);
    if (st == PICA_OK && e && *e == 's') {      /* simulate a wrong channel swizzle on hardware */
        uint32_t *w = dst, i;
        for (i = 0; i < bytes / 4; i++)
            w[i] = (w[i] & 0xFF00FF00u) | (w[i] >> 16 & 0xFF) | (w[i] & 0xFF) << 16;
    }
    pica_drv_free(&drv, 2, pa);
    return st;
}

const pica_dev_t *pica_dev_host(void)
{
    static const pica_dev_t dev = { "PICA200 software model (host)", d_info, d_alloc, d_free, d_write, d_read,
                                    d_submit, d_fill, d_transfer, d_config, d_present };
    return &dev;
}

const void *pica_dev_host_stats(void) { return &sim.st; }

const char *pica_strerror(uint32_t st)
{
    static const char *const s[] = { "ok", "bad request", "GPU is powered off / in reset", "no contiguous memory for the arena",
        "GPU arena full", "address outside the arena", "bad user pointer", "GPU timeout (see console log)",
        "no ios3ds_gpu sysctl service",
        "destination is not one physically contiguous, GPU-reachable, user-writable range" };
    return st < sizeof s / sizeof s[0] ? s[st] : "unknown error";
}
