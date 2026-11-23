/*
 * Host test of the GPU display mirror (src/pica_mirror.c + gpumirror.bin)
 * against the software PICA model, and against the ARM9's own CPU copy:
 * boot/arm9/display.c is compiled in as it is and run on the same picture.
 *
 *   test_mirror build/pica-host/gpumirror.bin
 *
 * display.c works on 32-bit physical addresses, so VRAM and the fake CLCD
 * page are mapped at their real addresses in this process (MAP_FIXED).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "pica_mirror.h"
#include "pica_drv.h"
#include "pica_diag.h"
#include "pica_regs.h"
#include "../sim/picasim.h"
#include "display.h"

#define VRAM_PA   0x18000000u
#define VRAM_SIZE 0x600000u
#define CLCD_PA   0x1FFB0000u
#define ARENA_PA  0x26000000u
#define ARENA_SZ  0x400000u
#define TEX_PA    (ARENA_PA + 0x200000u)      /* the mirror's 512x512 texture, as the shim takes it from the arena */
#define DRV_PA    0x27000000u           /* the GL driver core's arena: pica_drv_init programs the GX block */
#define DRV_SZ    0x40000u
/* Luma's framebuffers (docs/hwlogs: "framebuffers: top 0x18300000/0x18400000 bottom 0x18346500/0x18446500") */
#define TOP0 0x18300000u
#define TOP1 0x18400000u
#define BOT0 0x18346500u
#define BOT1 0x18446500u

static PicaSim sim;
static uint8_t *vram, *blob, *arena, *drv_arena;
static uint32_t blob_len, tick;
static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

/* display.c's outside dependencies (stereo is off in this test) */
bool blksrv_poll(void) { return false; }
bool gm_set_stereo(bool on) { (void)on; return true; }
void log_color(unsigned color, const char *fmt, ...) { (void)color; (void)fmt; }

static uint8_t *h_mem(void *o, uint32_t pa, uint32_t len, int write)
{
    (void)o; (void)write;
    if (pa >= VRAM_PA && len <= VRAM_SIZE && pa - VRAM_PA <= VRAM_SIZE - len)
        return vram + (pa - VRAM_PA);
    if (pa >= GM_BLOB_PA && len <= blob_len && pa - GM_BLOB_PA <= blob_len - len)
        return blob + (pa - GM_BLOB_PA);
    if (pa >= ARENA_PA && len <= ARENA_SZ && pa - ARENA_PA <= ARENA_SZ - len)
        return arena + (pa - ARENA_PA);
    if (pa >= DRV_PA && len <= DRV_SZ && pa - DRV_PA <= DRV_SZ - len)
        return drv_arena + (pa - DRV_PA);
    return NULL;
}
static void h_log(void *o, const char *msg) { (void)o; printf("  model: %s\n", msg); }

/* "Lazy" GPU: like the QEMU machine, an operation is not run when it is
   started but some register reads later, so the engine reads busy in
   between and pm_poll() has to come back for it, as on hardware. */
static int lazy;
static struct { uint32_t off, val, left; } q[8];
static int nq;
static uint32_t busy_polls;

static int trigger(uint32_t off, uint32_t v)
{
    return ((off == 0x01C || off == 0x02C || off == 0xC18) && (v & 1)) || off == 0x1000 + 0x23C * 4;
}
static uint32_t p_rd(uint32_t off)
{
    int i;
    for (i = 0; i < nq; i++)
        if (!--q[i].left) {
            picasim_write(&sim, q[i].off, q[i].val);
            q[i] = q[--nq];
            i--;
        }
    return picasim_read(&sim, off);
}
static void p_wr(uint32_t off, uint32_t v)
{
    if (lazy && trigger(off, v) && nq < 8) {
        q[nq].off = off;
        q[nq].val = v;
        q[nq].left = (uint32_t)lazy;
        nq++;
        return;
    }
    picasim_write(&sim, off, v);
}
static uint32_t p_ticks(void) { return tick++; }
static void p_sync(void *p, uint32_t len) { (void)p; (void)len; }
static void p_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    va_end(ap);
}
static const pm_plat_t plat = { p_rd, p_wr, p_ticks, 1000000, p_sync, p_sync, p_log, NULL };
static int d_copy(pica_uptr_t a, void *b, uint32_t n) { memcpy(b, (const void *)a, n); return 0; }
static int d_copyout(const void *a, pica_uptr_t b, uint32_t n) { memcpy((void *)b, a, n); return 0; }
static void d_udelay(uint32_t us) { (void)us; }
static int d_alive(uint32_t o) { (void)o; return 1; }
static const pica_plat_t dplat = { p_rd, p_wr, d_copy, d_copyout, p_sync, p_sync, d_udelay, d_alive, p_log };

static int run_frame(pica_mirror_t *m, const pm_frame_t *f)
{
    int st;
    pm_begin(m, f);
    while ((st = pm_poll(m, 0)) == PM_BUSY)     /* like the shim's interrupts: one look per call */
        busy_polls++;
    return st;
}

static void picture(uint32_t seed)
{
    uint32_t *p = (uint32_t *)vram, i;
    for (i = 0; i < GM_SRC_W * GM_SRC_H; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = seed ^ (seed >> 13) ^ i;
    }
}

#define FB_LO TOP0
#define FB_HI (BOT1 + GM_BOT_FB_BYTES)

static int compare_with_cpu(pica_mirror_t *m, int mode, const char *name)
{
    static uint8_t cpu[FB_HI - FB_LO];
    vu32 *clcd = (vu32 *)(uintptr_t)CLCD_PA;
    pm_frame_t f = { VRAM_PA, (uint32_t)mode, 2, { TOP0, TOP1 }, { BOT0, BOT1 } };
    uint32_t i, diff = 0, first = 0;

    memset(vram + (FB_LO - VRAM_PA), 0, FB_HI - FB_LO);
    display_set_mode(mode);
    CHECK(display_active(CLCD_PA), "display_active");
    display_blit(CLCD_PA);
    memcpy(cpu, vram + (FB_LO - VRAM_PA), sizeof cpu);
    (void)clcd;

    memset(vram + (FB_LO - VRAM_PA), 0, FB_HI - FB_LO);
    CHECK(run_frame(m, &f) == PM_DONE, "%s: frame did not finish", name);
    for (i = 0; i < sizeof cpu; i++)
        if (cpu[i] != vram[FB_LO - VRAM_PA + i]) {
            if (!diff)
                first = i;
            diff++;
        }
    CHECK(diff == 0, "%s: GPU and CPU copies differ in %u bytes, first at PA 0x%08x (cpu %02x gpu %02x)",
          name, diff, FB_LO + first, cpu[first], vram[FB_LO - VRAM_PA + first]);
    printf("mode %-6s: GPU mirror vs ARM9 CPU copy (display.c): %u of %u bytes differ; %u GPU ops\n",
           name, diff, (unsigned)sizeof cpu, m->nops);
    return (int)diff;
}

int main(int argc, char **argv)
{
    static const PicaSimHost host = { h_mem, NULL, h_log, NULL };
    pica_mirror_t m, bad;
    uint32_t bad0 = 0, fb[4], y, x, i, d;
    FILE *fp;
    vu32 *clcd;
    int v;

    if (argc != 2 || !(fp = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: test_mirror gpumirror.bin\n");
        return 2;
    }
    blob = calloc(1, GM_BLOB_MAX);
    blob_len = (uint32_t)fread(blob, 1, GM_BLOB_MAX, fp);
    fclose(fp);
    vram = mmap((void *)(uintptr_t)VRAM_PA, VRAM_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    clcd = mmap((void *)(uintptr_t)CLCD_PA, 0x1000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (vram != (void *)(uintptr_t)VRAM_PA || clcd != (void *)(uintptr_t)CLCD_PA) {
        fprintf(stderr, "test_mirror: cannot map VRAM / CLCD at their physical addresses\n");
        return 1;
    }
    arena = malloc(ARENA_SZ);
    memset(arena, 0xA5, ARENA_SZ);
    memset(vram, 0x77, VRAM_SIZE);
    picasim_init(&sim, &host);
    {   /* the shim brings the GPU up with the GL driver core first (end-of-list marker etc.) */
        static pica_drv_t drv;
        drv_arena = malloc(DRV_SZ);
        CHECK(pica_drv_init(&drv, &dplat, drv_arena, DRV_PA, DRV_SZ, 0x1007F) == PICA_OK, "pica_drv_init");
    }

    CHECK(pm_init(&m, &plat, blob, GM_BLOB_PA, TEX_PA) == 0, "pm_init rejects the blob");
    CHECK(pm_init(&bad, &plat, blob, GM_BLOB_PA + 0x1000, TEX_PA) != 0, "pm_init accepts a blob for another address");

    /* 1. self-test as the shim runs it */
    v = pm_selftest(&m, arena, ARENA_PA, arena + 0x100000, ARENA_PA + 0x100000, 4096, &bad0, fb);
    CHECK(v == 0 && bad0 == 0, "self-test: variant %d, %u bad bytes", v, bad0);
    printf("self-test: variant %d, %u bad bytes, %u GPU frames\n", v, bad0, m.frames);
    {   /* the failure dump's expectations are right: right after a passing self-test all 16 words match */
        pd_env_t e = { &plat, vram + (PD_VRAM_PA - VRAM_PA), arena + 0x180000, ARENA_PA + 0x180000, blob, GM_BLOB_PA };
        uint32_t same = pd_dump_mirror(&e, (const uint32_t *)(arena + (TEX_PA - ARENA_PA)));
        CHECK(same == 24, "pd_dump_mirror after a good self-test: %u of 24 words as expected", same);
    }

    /* 2. a broken list must be caught: both lists = the half-texel one */
    bad = m;
    bad.list_pa[0] = bad.list_pa[1];
    bad.list_bytes[0] = bad.list_bytes[1];
    v = pm_selftest(&bad, arena, ARENA_PA, arena + 0x100000, ARENA_PA + 0x100000, 4096, &bad0, fb);
    CHECK(v < 0 && bad0 > 0, "self-test missed a wrong texture mapping (variant %d, %u bad)", v, bad0);
    printf("self-test with the wrong sampling list: result %d, %u bad bytes (first at %u: %02x != %02x) - caught\n",
           v, bad0, fb[0], fb[2], fb[1]);

    /* 3. pixel-for-pixel against the CPU copy, all three screen modes */
    picture(1);
    clcd[0] = 1;
    clcd[0x24 / 4] = VRAM_PA;
    clcd[0x28 / 4] = GM_SRC_W;
    clcd[0x30 / 4] = GM_SRC_W << 16 | GM_SRC_H;
    display_init((u8 *)(uintptr_t)TOP0, (u8 *)(uintptr_t)TOP1, (u8 *)(uintptr_t)BOT0, (u8 *)(uintptr_t)BOT1);
    display_arm(CLCD_PA);
    clcd[0x08 / 4] ^= 1;            /* "the driver wrote to it" */
    display_fbs(fb);
    CHECK(fb[0] == TOP0 && fb[1] == TOP1 && fb[2] == BOT0 && fb[3] == BOT1, "display_fbs");
    m.variant = 0;
    CHECK(pm_clear_targets(&m) == 0, "pm_clear_targets");
    compare_with_cpu(&m, SCREEN_TOP, "top");
    compare_with_cpu(&m, SCREEN_BOTH, "both");
    compare_with_cpu(&m, SCREEN_BOTTOM, "bottom");
    picture(7);
    compare_with_cpu(&m, SCREEN_BOTH, "both#2");
    /* the CPU copy writes only the framebuffer set the LCDs show (PDC select,
       published by the shim): set 1 stays untouched, set 0 is the same */
    {
        static uint8_t ref[FB_HI - FB_LO];
        uint32_t d0 = 0, d1 = 0;
        memcpy(ref, vram + (FB_LO - VRAM_PA), sizeof ref);      /* the GPU's frame, both sets */
        memset(vram + (FB_LO - VRAM_PA), 0, FB_HI - FB_LO);
        display_set_shown(1 | 4);
        display_blit(CLCD_PA);
        display_set_shown(0xF);
        for (i = 0; i < GM_TOP_FB_BYTES + GM_BOT_FB_BYTES; i++) {
            d0 += vram[TOP0 - VRAM_PA + i] != ref[TOP0 - FB_LO + i];
            d1 += vram[TOP1 - VRAM_PA + i] != 0;
        }
        CHECK(d0 == 0 && d1 == 0, "shown = set 0: %u bytes of set 0 differ, %u bytes of set 1 written", d0, d1);
        printf("CPU copy into the shown set only: set 0 %u bytes differ, set 1 %u bytes written\n", d0, d1);
    }

    /* 4. the flip variants do what they say (under the model): read-out flip
       reverses each screen's framebuffer rows */
    {
        static uint8_t ref[GM_TOP_FB_BYTES + GM_BOT_FB_BYTES];
        pm_frame_t f = { VRAM_PA, 1, 1, { TOP0, TOP0 }, { BOT0, BOT0 } };
        memcpy(ref, vram + (TOP0 - VRAM_PA), sizeof ref);
        m.variant = GM_VAR_OUT_FLIP;
        CHECK(run_frame(&m, &f) == PM_DONE, "flip frame");
        for (d = 0, y = 0; y < GM_RT_TOP_H; y++)
            for (x = 0; x < GM_RT_W * 3; x++)
                d += ref[y * 720 + x] != vram[TOP0 - VRAM_PA + (GM_RT_TOP_H - 1 - y) * 720 + x];
        for (y = 0; y < GM_RT_BOT_H; y++)
            for (x = 0; x < GM_RT_W * 3; x++)
                d += ref[GM_TOP_FB_BYTES + y * 720 + x] != vram[BOT0 - VRAM_PA + (GM_RT_BOT_H - 1 - y) * 720 + x];
        CHECK(d == 0, "GM_VAR_OUT_FLIP: %u bytes are not the row-reversed picture", d);
        printf("read-out flip variant: %u bytes differ from the row-reversed frame (%u ops: per-screen transfers)\n",
               d, m.nops);
        m.variant = 0;
    }

    /* 4a. the staged hardware diagnostics (pica_diag.c) pass on the model */
    {
        pd_env_t e = { &plat, vram + (PD_VRAM_PA - VRAM_PA), arena + 0x180000, ARENA_PA + 0x180000, blob, GM_BLOB_PA };
        /* FCRAM PSC fills still fail (a hardware rule, run 13); the NPOT
           check now uploads a padded POT texture with scaled texcoords, the
           same workaround the GL back end uses, so it passes on hardware. */
        uint32_t r, all = PD_FILL_VRAM | PD_PPF | PD_SOLID_VRAM | PD_SOLID_FCRAM | PD_TEX_POT | PD_TEX_NPOT;
        pd_dump_regs(&e, "host");
        r = pd_fill_tests(&e, "host") | pd_gpu_tests(&e);
        CHECK(r == all, "diagnostics on the model: 0x%x, hardware run 13 gave 0x%x", r, all);
        pd_dump_mirror(&e, NULL);
        printf("diagnostics on the model: 0x%x (hardware run 13: 0x%x)\n", r, all);
    }

    /* 4b. the same with a GPU that takes its time: frames need several steps */
    lazy = 7;
    busy_polls = 0;
    v = pm_selftest(&m, arena, ARENA_PA, arena + 0x100000, ARENA_PA + 0x100000, 4096, &bad0, fb);
    CHECK(v == 0 && bad0 == 0, "lazy GPU: self-test variant %d, %u bad bytes", v, bad0);
    CHECK(pm_clear_targets(&m) == 0, "lazy GPU: pm_clear_targets");
    picture(3);
    compare_with_cpu(&m, SCREEN_TOP, "top/lazy");
    compare_with_cpu(&m, SCREEN_BOTH, "both/lazy");
    compare_with_cpu(&m, SCREEN_BOTTOM, "bottom/lazy");
    CHECK(busy_polls > 3 * 4, "lazy GPU: frames finished without stepping (%u busy polls)", busy_polls);
    printf("lazy GPU: self-test variant %d; 3 frames needed %u busy steps in between\n", v, busy_polls);
    lazy = 0;
    CHECK(sim.st.errors == 0, "the model logged %u errors", sim.st.errors);

    /* 5. timeouts are reported, not waited for (the model complains about the address: expected) */
    {
        pm_frame_t f = { VRAM_PA, 1, 1, { TOP0, TOP0 }, { BOT0, BOT0 } };
        pica_mirror_t t = m;
        uint32_t r;
        t.list_pa[0] = 0x30000000;              /* the model cannot reach it: no end marker */
        pm_begin(&t, &f);
        for (r = 0, i = 0; i < 1000000 && (r = (uint32_t)pm_poll(&t, 0)) == PM_BUSY; i++)
            ;
        CHECK(r == PM_TIMEOUT && t.cur == 2, "unreachable list: state %u at op %u", r, t.cur);
        /* the self-test reports a GPU that does not finish as such, not as a mismatch (soak4) */
        {
            uint32_t b0 = 7, fb4[4] = { 0, 0, 0, 0 };
            int sv = pm_selftest(&t, arena, ARENA_PA, arena + 0x100000, ARENA_PA + 0x100000, 4096, &b0, fb4);
            CHECK(sv == PM_SELFTEST_TIMEOUT && fb4[0] == 3 && b0 == 0,
                  "self-test with a hung GPU: result %d, operation %u, bad %u", sv, fb4[0], b0);
            printf("self-test with a hung GPU: result %d (PM_SELFTEST_TIMEOUT), operation %u\n", sv, fb4[0]);
        }
        printf("unreachable command list: pm_poll -> %s at operation %u after %u polls\n",
               r == PM_TIMEOUT ? "TIMEOUT" : "?", t.cur + 1, i);
    }
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
