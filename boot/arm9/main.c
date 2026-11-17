/*
 * ios3ds ARM9 monitor (M1).
 *
 *  1. text consoles on both screens (framebuffers from Luma's screen init)
 *  2. sd:/ios3ds/log.txt (previous boot kept as log_prev.txt)
 *  3. handshake with the ARM11 stub, print its CPU ID registers + probe results
 *  4. read sd:/ios3ds/boot.cfg, load kernel.bin + devtree.bin into FCRAM
 *  5. build XNU boot_args, tell the ARM11 to jump into XNU
 *  6. stay alive as the monitor: forward ARM11/XNU log rings to screen + SD
 *
 * Every step logs before it starts, so a hang shows where it happened.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "util.h"
#include "console.h"
#include "log.h"
#include "blksrv.h"
#include "display.h"
#include "tsc.h"
#include "swap.h"
#include "wifi/wifi.h"
#include "audio.h"

#define CFG9_SDMMCCTL (*(vu16 *)0x10000020u)

extern void waitcycles(u32 n);
extern u32 a9_get_cpsr(void);

/* ~1 ms of ARM9 busy-wait at 134 MHz (4 cycles per waitcycles iteration) */
static void delay_ms(u32 ms)
{
    while (ms--) waitcycles(33500);
}

static const char *stage_name(u32 s)
{
    static const char *n[] = { "none", "entry", "id-regs", "probes", "vfp", "ready", "jumping" };
    return s < ARRAY_SIZE(n) ? n[s] : "?";
}

/* ------------------------------------------------------------------------ */
/* bottom screen status                                                     */

static void status(int row, u32 color, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_line(CON_BOTTOM, row, color, buf);
}

/* ------------------------------------------------------------------------ */
/* ARM11 log ring                                                           */

static u32 a11_log_tail;

/* copy ring bytes [*tail, head) to the log in chunks: one f_write per chunk,
   not per byte (per-byte writes + syncs made real-SD console output crawl
   and starved the md0 block server running on the same ARM9) */
static void drain_ring(const volatile char *buf, u32 size, u32 *tail, u32 head, u32 color)
{
    char chunk[512];
    while (*tail != head) {
        u32 n = 0;
        while (*tail != head && n < sizeof(chunk)) {
            chunk[n++] = buf[*tail % size];
            (*tail)++;
        }
        log_raw(chunk, n, color);
    }
}

static void drain_a11_log(void)
{
    volatile shared_t *sh = SHARED;
    u32 head = sh->log_head;
    if (head - a11_log_tail > SHARED_LOG_SIZE) {
        log_warn("[a11] (log overrun, %u bytes lost)\n", head - a11_log_tail - SHARED_LOG_SIZE);
        a11_log_tail = head - SHARED_LOG_SIZE;
    }
    drain_ring((const volatile char *)sh->log_buf, SHARED_LOG_SIZE, &a11_log_tail, head, CON_CYAN);
}

/* ------------------------------------------------------------------------ */
/* boot.cfg                                                                 */

typedef struct {
    char kernel[64], devtree[64], shim[64], rootfs[64];
    u32 shim_pa;
    u32 kernel_pa, entry_pa, devtree_pa, bootargs_pa, ring_pa, ring_size, top_pa;
    u32 phys_base, virt_base, mem_size;
    u32 video_pa, video_display, video_rowbytes, video_width, video_height, video_depth;
    u32 jump, countdown_ms;
    u32 ba_revision, ba_version;
    u32 probes;
    u32 display_regs;
    u32 screen;          /* SCREEN_* start mode; X cycles it at run time */
    u32 nloads;
    char load_path[4][64];
    u32 load_pa[4];
    char cmdline[256];
} bootcfg_t;

static bootcfg_t cfg;

static const char *const screen_names[SCREEN_MODES] = { "top", "both", "bottom" };

/* where iOS and the monitor log go (display.h SCREEN_*) */
static void apply_screen_mode(int m)
{
    display_set_mode(m);
    con_show(CON_TOP, m == SCREEN_BOTTOM);
    con_show(CON_BOTTOM, m == SCREEN_TOP);
    log_target(m == SCREEN_TOP ? CON_BOTTOM : m == SCREEN_BOTTOM ? CON_TOP : -1);
    log_color(CON_CYAN, "[monitor] screen mode: %s (X = next)\n", screen_names[m]);
}

static int parse_bootcfg(char *txt)
{
    memset(&cfg, 0, sizeof(cfg));
    strlcpy_(cfg.kernel, "0:/ios3ds/kernel.bin", sizeof(cfg.kernel));
    strlcpy_(cfg.devtree, "0:/ios3ds/devtree.bin", sizeof(cfg.devtree));
    cfg.jump = 1;
    cfg.countdown_ms = 3000;
    cfg.phys_base = FCRAM_BASE;
    cfg.virt_base = 0x80000000u;
    cfg.mem_size = FCRAM_SIZE;
    cfg.ba_revision = 1;
    cfg.probes = 1;
    cfg.ba_version = 5;
    cfg.screen = SCREEN_BOTH;   /* pe_identify_machine panics 'Epoch Mismatch' unless == 5 */

    static const struct { const char *k; u32 *v; } nums[] = {
        { "kernel_pa", &cfg.kernel_pa }, { "entry_pa", &cfg.entry_pa },
        { "devtree_pa", &cfg.devtree_pa }, { "bootargs_pa", &cfg.bootargs_pa },
        { "shim_pa", &cfg.shim_pa }, { "ring_pa", &cfg.ring_pa }, { "ring_size", &cfg.ring_size }, { "top_pa", &cfg.top_pa },
        { "phys_base", &cfg.phys_base }, { "virt_base", &cfg.virt_base }, { "mem_size", &cfg.mem_size },
        { "video_pa", &cfg.video_pa }, { "video_display", &cfg.video_display },
        { "video_rowbytes", &cfg.video_rowbytes }, { "video_width", &cfg.video_width },
        { "video_height", &cfg.video_height }, { "video_depth", &cfg.video_depth },
        { "jump", &cfg.jump }, { "countdown_ms", &cfg.countdown_ms },
        { "probes", &cfg.probes }, { "display_regs", &cfg.display_regs }, { "ba_revision", &cfg.ba_revision }, { "ba_version", &cfg.ba_version },
        { "screen", &cfg.screen },
    };
    int lineno = 0;
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *eq = strchr(line, '=');
            if (!eq) {
                log_warn("boot.cfg:%d: no '=' - ignored\n", lineno);
            } else {
                *eq = 0;
                const char *key = line, *val = eq + 1;
                bool known = false;
                if (!strncmp(key, "cmdline", 8)) {
                    strlcpy_(cfg.cmdline, val, sizeof(cfg.cmdline));
                    known = true;
                } else if (!strncmp(key, "kernel", 7)) {
                    strlcpy_(cfg.kernel, val, sizeof(cfg.kernel));
                    known = true;
                } else if (!strncmp(key, "load", 5)) {
                    const char *comma = strchr(val, ',');
                    if (!comma || cfg.nloads >= 4) {
                        log_warn("boot.cfg:%d: load=path,pa (max 4)\n", lineno);
                    } else {
                        u32 n = comma - val < 63 ? comma - val : 63;
                        memcpy(cfg.load_path[cfg.nloads], val, n);
                        cfg.load_path[cfg.nloads][n] = 0;
                        cfg.load_pa[cfg.nloads] = parse_u32(comma + 1, NULL);
                        cfg.nloads++;
                    }
                    known = true;
                } else if (!strncmp(key, "rootfs", 7)) {
                    strlcpy_(cfg.rootfs, val, sizeof(cfg.rootfs));
                    known = true;
                } else if (!strncmp(key, "shim", 5)) {
                    strlcpy_(cfg.shim, val, sizeof(cfg.shim));
                    known = true;
                } else if (!strncmp(key, "devtree", 8)) {
                    strlcpy_(cfg.devtree, val, sizeof(cfg.devtree));
                    known = true;
                } else {
                    for (u32 i = 0; i < ARRAY_SIZE(nums); i++) {
                        if (!strncmp(key, nums[i].k, strlen(nums[i].k) + 1)) {
                            *nums[i].v = parse_u32(val, NULL);
                            known = true;
                        }
                    }
                }
                if (!known) log_warn("boot.cfg:%d: unknown key '%s'\n", lineno, key);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

static bool in_fcram(u32 pa, u32 len)
{
    return pa >= FCRAM_BASE && len <= FCRAM_SIZE && pa - FCRAM_BASE <= FCRAM_SIZE - len;
}

static bool validate_cfg(void)
{
    bool ok = true;
#define CHECK(cond, ...) do { if (!(cond)) { log_err(__VA_ARGS__); ok = false; } } while (0)
    CHECK(cfg.phys_base == FCRAM_BASE, "phys_base must be 0x%08x\n", FCRAM_BASE);
    CHECK((cfg.phys_base & 0xFFFFFF) == 0, "phys_base not 16 MiB aligned\n");
    CHECK(cfg.mem_size && (cfg.mem_size & 0xFFFFF) == 0 && cfg.mem_size <= FCRAM_SIZE,
          "mem_size must be a non-zero multiple of 1 MiB <= 128 MiB (XNU loops forever otherwise)\n");
    CHECK(in_fcram(cfg.kernel_pa, 0x1000), "kernel_pa outside FCRAM\n");
    CHECK(cfg.entry_pa > cfg.kernel_pa && cfg.entry_pa < cfg.devtree_pa, "entry_pa not inside kernel\n");
    CHECK(cfg.devtree_pa > cfg.kernel_pa && cfg.bootargs_pa > cfg.devtree_pa, "region order wrong\n");
    CHECK(cfg.ring_pa >= cfg.bootargs_pa + sizeof(boot_args_t), "ring overlaps boot_args\n");
    CHECK(!cfg.shim[0] || (cfg.shim_pa >= cfg.bootargs_pa + sizeof(boot_args_t) && cfg.shim_pa < cfg.ring_pa),
          "shim_pa must lie between boot_args and the ring\n");
    CHECK(cfg.top_pa >= cfg.ring_pa + cfg.ring_size, "ring above topOfKernelData\n");
    CHECK((cfg.top_pa & 0x3FFF) == 0, "top_pa must be 16 KiB aligned (it becomes TTBR)\n");
    CHECK(cfg.top_pa + 0x9000 <= cfg.phys_base + cfg.mem_size, "page tables beyond mem_size\n");
#undef CHECK
    return ok;
}

/* ------------------------------------------------------------------------ */
/* ARM11 handshake                                                          */

static bool wait_arm11_ready(u32 timeout_ms)
{
    volatile shared_t *sh = SHARED;
    u32 token = sh->a9_token + 1;
    if (!token) token = 1;
    for (u32 t = 0; t < timeout_ms; t++) {
        sh->a9_token = token;
        if (sh->a11_echo == token && sh->magic == SHARED_MAGIC && sh->a11_stage == A11_STAGE_READY) {
            return true;
        }
        drain_a11_log();
        delay_ms(1);
    }
    return false;
}

static void report_cpu(void)
{
    volatile shared_t *sh = SHARED;
    log_color(CON_CYAN, "ARM11 core0 CP15 ID registers:\n");
    logp(" MIDR  %08x  CTR   %08x  TLBTR %08x\n", sh->midr, sh->ctr, sh->tlbtr);
    logp(" MPIDR %08x  PFR0  %08x  PFR1  %08x\n", sh->mpidr, sh->id_pfr0, sh->id_pfr1);
    logp(" DFR0  %08x  AFR0  %08x\n", sh->id_dfr0, sh->id_afr0);
    logp(" MMFR0-3 %08x %08x %08x %08x\n", sh->id_mmfr[0], sh->id_mmfr[1], sh->id_mmfr[2], sh->id_mmfr[3]);
    logp(" ISAR0-2 %08x %08x %08x\n", sh->id_isar[0], sh->id_isar[1], sh->id_isar[2]);
    logp(" ISAR3-5 %08x %08x %08x\n", sh->id_isar[3], sh->id_isar[4], sh->id_isar[5]);
    logp(" SCTLR %08x  ACTLR %08x  CPACR %08x  CPSR %08x\n", sh->sctlr, sh->actlr, sh->cpacr, sh->cpsr);
    u32 midr = sh->midr;
    logp(" -> part %03x rev r%up%u (%s)\n", (midr >> 4) & 0xFFF, (midr >> 20) & 0xF, midr & 0xF,
         ((midr >> 4) & 0xFFF) == 0xB02 ? "ARM11 MPCore" : "unexpected!");
}

static void report_probes(void)
{
    volatile shared_t *sh = SHARED;
    log_color(CON_CYAN, "Instruction probes (XNU uses these):\n");
    for (u32 i = 0; i < sh->nprobes && i < PROBE_MAX; i++) {
        volatile probe_t *p = &sh->probe[i];
        if (p->result == PROBE_OK) {
            log_ok(" %08x ok      -> %08x\n", p->encoding, p->value);
        } else if (p->result == PROBE_EXCEPTION) {
            log_warn(" %08x TRAPPED (slot %u mode %02x)\n", p->encoding, p->exc_slot, p->exc_mode);
        } else {
            log_err(" %08x not run\n", p->encoding);
        }
    }
    if (sh->unexpected_count) {
        log_err(" %u unexpected ARM11 exception(s): slot %u mode %02x lr %08x\n",
                sh->unexpected_count, sh->unexpected_slot, sh->unexpected_mode, sh->unexpected_lr);
    }
}

/* ------------------------------------------------------------------------ */
/* hang probes: everything the shim recorded, written to log.txt once the     */
/* ARM11 stops reading the clock, so no photo transcription is needed.       */

static u32 fc_rd(u32 pa) { return in_fcram(pa, 4) ? *(vu32 *)pa : 0xBAD0BAD0u; }

/* wall clock: ARM9 timer 2 at 67027964/1024 Hz cascading into timer 3 (32 bit,
   wraps after ~18 h). Timer 0 is the Wi-Fi code's entropy source; 1 is free. */
#define T9_VAL(n) (*(vu16 *)(0x10003000u + (n) * 4))
#define T9_CNT(n) (*(vu16 *)(0x10003002u + (n) * 4))
#define T9_HZ     65457u

static void clk_start(void)
{
    T9_CNT(2) = 0;
    T9_CNT(3) = 0;
    T9_VAL(2) = 0;
    T9_VAL(3) = 0;
    T9_CNT(3) = 0x80 | 0x04;            /* enable, count-up (cascade) */
    T9_CNT(2) = 0x80 | 0x03;            /* enable, prescaler 1024 */
}

static u32 clk_ticks(void)
{
    u16 hi, lo, hi2;
    do {
        hi = T9_VAL(3);
        lo = T9_VAL(2);
        hi2 = T9_VAL(3);
    } while (hi != hi2);
    return (u32)hi << 16 | lo;
}

static u32 clk_secs(void) { return clk_ticks() / T9_HZ; }

/* heartbeat while the console is quiet: is the kernel alive (clock reads,
   ticks), is md0 busy, and where does the ARM11 spend its time (IRQ-time PC
   samples, shim+0x5F80, 16 x {pc, spsr}, newest = st_pc_idx - 1) */
static void heartbeat(u32 quiet_s)
{
    vu32 *st = (vu32 *)cfg.shim_pa;
    vu32 *ring = (vu32 *)(cfg.shim_pa + 0x5F80);
    u32 bs, be, n = st[0x9C / 4];
    blksrv_stats(&bs, &be);
    logp("[hb] t=%us quiet %us | abs %u dec %u timer %u irq %u vbl %u | md0 %u req %u KiB %u err\n",
         clk_secs(), quiet_s, (unsigned)st[0x80 / 4], (unsigned)st[0x8C / 4], (unsigned)st[0x4C / 4],
         (unsigned)st[0x98 / 4], (unsigned)st[0x78 / 4], bs, blksrv_sectors() / 2, be);
    char line[200];
    int len = snprintf(line, sizeof(line), "[hb]  pc");
    for (u32 i = 1; i <= 8 && i <= n; i++) {
        u32 k = (n - i) & 15, pc = ring[k * 2], psr = ring[k * 2 + 1];
        const char *m = (psr & 0x1F) == 0x10 ? "u" : (psr & 0x1F) == 0x13 ? "k" : "x";
        len += snprintf(line + len, sizeof(line) - len, " %s%08x%s", m, pc, (psr & 0x20) ? "t" : "");
        if (len >= (int)sizeof(line)) break;
    }
    logp("%s\n", line);
}

static void dump_probes(const char *why)
{
    vu32 *st = (vu32 *)cfg.shim_pa;
    vu32 *g = (vu32 *)(cfg.shim_pa + 0x5F00);
    u32 v[8];

    log_warn("[probe] %s\n", why);
    logp("[probe] shim init%u timer%u other%u last%u def%u vbl%u inj%u irq-entries%u\n",
         (unsigned)st[0x40 / 4], (unsigned)st[0x4C / 4], (unsigned)st[0x50 / 4], (unsigned)st[0x54 / 4],
         (unsigned)st[0x58 / 4], (unsigned)st[0x78 / 4], (unsigned)st[0x7C / 4], (unsigned)st[0x98 / 4]);
    logp("[probe] abs %u raw %08x cpsr %08x | dec %u last %u caller %08x\n",
         (unsigned)st[0x80 / 4], (unsigned)st[0x84 / 4], (unsigned)st[0x88 / 4],
         (unsigned)st[0x8C / 4], (unsigned)st[0x90 / 4], (unsigned)st[0x94 / 4]);
    logp("[probe] at the last clock read: ptimer count %08x ctrl %08x isr %x\n",
         (unsigned)g[0], (unsigned)g[1], (unsigned)g[2]);
    logp("[probe]  gicc ctrl %x pmr %x hppir %x | gicd ctrl %x en %08x/%08x pend %08x/%08x act %08x\n",
         (unsigned)g[3], (unsigned)g[4], (unsigned)g[5], (unsigned)g[6], (unsigned)g[7],
         (unsigned)g[10], (unsigned)g[8], (unsigned)g[9], (unsigned)g[11]);
    logp("[probe]  sctlr %08x ttbr0 %08x ttbr1 %08x ttbcr %08x dacr %08x mpidr %08x\n",
         (unsigned)g[12], (unsigned)g[13], (unsigned)g[14], (unsigned)g[15],
         (unsigned)g[0x60 / 4], (unsigned)g[0x64 / 4]);
    logp("[probe]  a11 walk stage %u: L1[fff] %08x L2[f0] %08x irq-literal %08x handler %08x %08x\n",
         (unsigned)g[0x7C / 4], (unsigned)g[0x68 / 4], (unsigned)g[0x6C / 4],
         (unsigned)g[0x70 / 4], (unsigned)g[0x74 / 4], (unsigned)g[0x78 / 4]);
    for (int i = 0; i < 8; i++) v[i] = g[0x40 / 4 + i];
    logp("[probe]  a11 vectors %08x %08x %08x %08x %08x %08x %08x %08x\n",
         v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);

    /* the same walk through physical memory (what the table walker and an
       uncached instruction fetch see if the ARM11 did not clean its D-cache) */
    u32 ttbr1 = g[14];
    if (!ttbr1) {
        log_warn("[probe] no TTBR1 recorded (snapshot never ran)\n");
        return;
    }
    u32 l1 = fc_rd((ttbr1 & ~0x3FFFu) + 0xFFF * 4);
    u32 l2 = (l1 & 3) == 1 ? fc_rd((l1 & ~0x3FFu) + 0xF0 * 4) : 0;
    u32 vpa = (l2 & 2) ? (l2 & ~0xFFFu) : 0;
    u32 w[8], lit = 0, h0 = 0, h1 = 0;
    for (int i = 0; i < 8; i++) w[i] = vpa ? fc_rd(vpa + i * 4) : 0;
    if ((w[6] >> 12) == 0xE59FFu) {
        lit = fc_rd(vpa + 0x20 + (w[6] & 0xFFF));
        u32 hpa = lit - cfg.virt_base + cfg.phys_base;
        h0 = fc_rd(hpa);
        h1 = fc_rd(hpa + 4);
    }
    logp("[probe]  a9  walk: L1[fff] %08x L2[f0] %08x vector page PA %08x irq-literal %08x handler %08x %08x\n",
         l1, l2, vpa, lit, h0, h1);
    logp("[probe]  a9  vectors %08x %08x %08x %08x %08x %08x %08x %08x\n",
         w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    bool same = l1 == g[0x68 / 4] && l2 == g[0x6C / 4];
    for (int i = 0; i < 8; i++) same = same && w[i] == v[i];
    if (g[0x7C / 4] >= 3) same = same && lit == g[0x70 / 4];
    if (g[0x7C / 4] >= 4) same = same && h0 == g[0x74 / 4] && h1 == g[0x78 / 4];
    if (same) log_ok("[probe]  ARM11 view == memory (vector page and tables are clean)\n");
    else log_err("[probe]  ARM11 view != memory: stale D-cache or a different mapping\n");
}

/* ------------------------------------------------------------------------ */
/* monitor                                                                  */

static void __attribute__((noreturn)) monitor(const char *phase)
{
    volatile xnu_ring_t *ring = cfg.ring_pa ? (volatile xnu_ring_t *)cfg.ring_pa : NULL;
    u32 ring_tail = 0;
    u32 tick = 0;
    bool l1_seen = false;
    u32 *l1 = cfg.top_pa ? (u32 *)cfg.top_pa : NULL;

    log_color(CON_GREY, "[monitor] %s - ARM9 stays here forwarding logs.\n", phase);
    clk_start();
    /* sync the log every 0.5 s, not after every write: a power cut loses at
       most 0.5 s, a hang loses nothing (the ARM9 keeps syncing) */
    log_sync_each(false);
    u32 last_sync = 0;
    for (;;) {
        bool busy = false;
        for (int i = 0; i < 64 && blksrv_poll(); i++) busy = true;
        drain_a11_log();
        if (ring && ring->magic == XNU_RING_MAGIC && ring->size) {
            u32 head = ring->head;
            if (head - ring_tail > ring->size) {
                log_warn("[xnu] (ring overrun, %u bytes lost)\n", head - ring_tail - ring->size);
                ring_tail = head - ring->size;
            }
            drain_ring((const volatile char *)ring->buf, ring->size, &ring_tail, head, CON_WHITE);
        }
        /* The entry stub zeroes 36 KiB at topOfKernelData and writes the L1
           entries for virtBase. We filled it with a pattern before the jump. */
        if (l1 && !l1_seen) {
            u32 idx = cfg.virt_base >> 20;
            u32 e = l1[idx];
            if (e != 0xA5A5A5A5u && e != 0) {
                l1_seen = true;
                log_ok("[monitor] XNU entry stub ran: L1[0x%x] = %08x (TTBR region %08x)\n", idx, e, cfg.top_pa);
            }
        }
        /* 3DS pad + touch -> shim input ring (read by /usr/libexec/ios3ds-input) */
        input_poll(cfg.shim_pa);
        /* Wi-Fi driver step + frames <-> network mailbox (kern.ios3ds_net) */
        if (wifi_poll()) busy = true;
        audio_poll(cfg.shim_pa);    /* docs/audio.md */
        static bool ios_display, x_latched;
        static int screen_mode;
        /* screen copy pacing: <= 15 fps; while md0 is busy (a request in the
           last 100 ms) only every 500 ms, because the ARM11 kernel stalls on
           each block request until this loop serves it */
        static u32 last_blit, last_io, last_served;
        u32 nserved, nerr, tnow = clk_ticks();
        blksrv_stats(&nserved, &nerr);
        if (nserved != last_served) {
            last_served = nserved;
            last_io = tnow;
        }
        bool io_busy = tnow - last_io < T9_HZ / 10;
        u32 blit_gap = io_busy ? T9_HZ / 2 : T9_HZ / 15;
        if (cfg.display_regs && tnow - last_blit >= blit_gap && display_active(cfg.display_regs)) {
            if (!ios_display) {
                ios_display = true;
                log_color(CON_GREEN, "[monitor] iOS display is on: top screen now mirrors the CLCD framebuffer\n");
                screen_mode = cfg.screen < SCREEN_MODES ? (int)cfg.screen : SCREEN_BOTH;
                apply_screen_mode(screen_mode);
            }
            display_blit(cfg.display_regs);
            last_blit = clk_ticks();
        }
        /* X: next screen mode (X is not forwarded to iOS as a button) */
        bool xdown = !(HID_PAD & HID_X);
        if (ios_display && xdown && !x_latched) {
            screen_mode = (screen_mode + 1) % SCREEN_MODES;
            apply_screen_mode(screen_mode);
        }
        x_latched = xdown;
        static bool combo_latched;
        bool combo = !(HID_PAD & HID_SELECT) && !(HID_PAD & HID_START);
        if (cfg.shim_pa && combo && !combo_latched) {
            log_warn("[monitor] SELECT+START: asking XNU to panic (thread dump)\n");
            *(vu32 *)(cfg.shim_pa + SHIM_PANIC_REQ) = 1;
        }
        combo_latched = combo;
        if ((tick & 63) == 0) {
            u32 bs, be;
            blksrv_stats(&bs, &be);
            status(27, CON_GREY, "monitor alive #%u a11=%s", tick / 64, stage_name(SHARED->a11_stage));
            status(28, CON_GREY, "xnu ring: %u bytes", ring ? (unsigned)ring->head : 0);
            status(29, CON_GREY, "md0: %u req, %u err", bs, be);
            if (cfg.shim_pa) {          /* shim stats (kernel/shim/entry.S +0x40..) */
                vu32 *st = (vu32 *)cfg.shim_pa;
                status(26, CON_GREY, "shim i%u t%u o%u L%u d%u v%u j%u", (unsigned)st[0x40 / 4],
                       (unsigned)st[0x4C / 4], (unsigned)st[0x50 / 4], (unsigned)st[0x54 / 4] & 0x3FF,
                       (unsigned)st[0x58 / 4], (unsigned)st[0x78 / 4], (unsigned)st[0x7C / 4]);
                /* clock probes: reads/raw watchdog/CPSR, deadline sets/last/caller */
                status(24, CON_GREY, "abs %u w%08x c%03x", (unsigned)st[0x80 / 4],
                       (unsigned)st[0x84 / 4], (unsigned)st[0x88 / 4] & 0x1FF);
                status(25, CON_GREY, "dec %u l%u @%08x", (unsigned)st[0x8C / 4],
                       (unsigned)st[0x90 / 4], (unsigned)st[0x94 / 4]);
                /* timer + GIC snapshot from the last clock read (shim+0x5F00) */
                vu32 *g = (vu32 *)(cfg.shim_pa + 0x5F00);
                status(21, CON_GREY, "irq %u pt %x c%x s%x", (unsigned)st[0x98 / 4],
                       (unsigned)g[0], (unsigned)g[1], (unsigned)g[2]);
                status(22, CON_GREY, "cc%x pm%x hp%x dc%x act%x", (unsigned)g[3],
                       (unsigned)g[4], (unsigned)g[5], (unsigned)g[6], (unsigned)g[11]);
                status(23, CON_GREY, "en%08x/%08x pd%08x/%08x", (unsigned)g[7],
                       (unsigned)g[10], (unsigned)g[8], (unsigned)g[9]);
                status(20, CON_GREY, "sctlr %08x stage %u", (unsigned)g[12], (unsigned)g[0x7C / 4]);
                /* 5 s without a clock read or console output: dump once.
                   10 s without console output: heartbeat every 10 s. */
                static u32 last_abs, last_head, abs_since, head_since, next_hb;
                static bool dumped;
                u32 now = clk_secs();
                u32 abs = st[0x80 / 4], hd = ring ? ring->head : 0;
                if (abs != last_abs || hd != last_head) {
                    abs_since = now;
                    dumped = false;
                }
                if (hd != last_head) {
                    head_since = now;
                    next_hb = now + 10;
                }
                last_abs = abs;
                last_head = hd;
                if (st[0x40 / 4] && !dumped && now - abs_since >= 5) {
                    dumped = true;
                    dump_probes("ARM11 stalled: no clock read or console output for 5 s");
                }
                if (st[0x40 / 4] && (int)(now - next_hb) >= 0) {
                    next_hb = now + 10;
                    heartbeat(now - head_since);
                }
            }
        }
        u32 now_t = clk_ticks();
        if (now_t - last_sync >= T9_HZ / 2) {
            log_flush();
            last_sync = now_t;
        }
        tick++;
        /* ~2 ms of mailbox polling instead of a blind delay: block requests
           are latency-bound (the ARM11 spins while it waits) */
        for (u32 i = 0; !busy && i < 200; i++) {
            if (blksrv_poll()) busy = true;
            else waitcycles(335);
        }
    }
}

/* ------------------------------------------------------------------------ */

static char cfgbuf[4096];

void a9_main(int argc, char **argv)
{
    u8 *top0 = NULL, *top1 = NULL, *bot0 = NULL, *bot1 = NULL;
    char why[96];

    /* argv lives on Luma's ITCM stack: copy what we need first */
    if (argc >= 2 && argv && argv[1]) {
        u32 *fbs = (u32 *)argv[1];
        top0 = (u8 *)fbs[0];
        bot0 = (u8 *)fbs[2];
        top1 = (u8 *)fbs[3];
        bot1 = (u8 *)fbs[5];
    }
    con_init(top0, top1, bot0, bot1);
    display_init(top0, top1, bot0, bot1);

    status(0, CON_GREEN, "ios3ds %s", IOS3DS_VERSION);
    status(1, CON_GREY, "M1 loader - ARM9 monitor");
    status(3, CON_WHITE, "log: sd:/ios3ds/log.txt");
    status(4, CON_WHITE, "hold B during countdown = no jump");
    status(5, CON_WHITE, "hold POWER to switch off");

    log_color(CON_GREEN, "ios3ds %s  (M1 loader)\n", IOS3DS_VERSION);
    logp("ARM9: argc=%d cpsr=%08x\n", argc, a9_get_cpsr());
    if (top0) {
        logp("framebuffers: top %p/%p bottom %p/%p\n", top0, top1, bot0, bot1);
    } else {
        log_warn("no framebuffers from loader (screen-init flag ignored?) - SD log only\n");
    }

    /* SD slot -> TMIO1 (ARM9), card power on; log the before/after values */
    u16 sdctl = CFG9_SDMMCCTL;
    CFG9_SDMMCCTL = (sdctl & ~1u) | 0x200u;
    logp("CFG9_SDMMCCTL %04x -> %04x\n", sdctl, CFG9_SDMMCCTL);

    logp("mounting SD...\n");
    if (log_mount_sd(why, sizeof(why)) != 0) {
        log_err("SD: %s\n", why);
        status(7, CON_RED, "SD MOUNT FAILED - see top screen");
    } else {
        log_ok("SD mounted, logging to sd:/ios3ds/log.txt\n");
        status(7, CON_GREEN, "SD ok");
        firm_swap_restore();            /* launcher (.3dsx) boot: Luma back first */
    }

    logp("waiting for ARM11 (stage %s)...\n", stage_name(SHARED->a11_stage));
    if (!wait_arm11_ready(3000)) {
        volatile shared_t *sh = SHARED;
        log_err("ARM11 not ready after 3 s: stage=%s probe=%u magic=%08x echo=%08x\n",
                stage_name(sh->a11_stage), sh->a11_current_probe, sh->magic, sh->a11_echo);
        status(8, CON_RED, "ARM11 STUCK at stage %s", stage_name(sh->a11_stage));
        report_cpu();
        monitor("ARM11 did not come up");
    }
    log_ok("ARM11 ready\n");
    status(8, CON_GREEN, "ARM11 ok, MIDR %08x", SHARED->midr);
    report_cpu();
    drain_a11_log();

    if (!log_sd_ok()) {
        log_err("no SD card: cannot load the kernel\n");
        monitor("no SD");
    }

    logp("reading sd:/ios3ds/boot.cfg\n");
    int n = sd_read_small("0:/ios3ds/boot.cfg", cfgbuf, sizeof(cfgbuf));
    if (n < 0) {
        log_err("boot.cfg missing (FRESULT %d). Run 'make sd' on the PC.\n", -n);
        status(9, CON_RED, "NO boot.cfg");
        monitor("nothing to boot");
    }
    parse_bootcfg(cfgbuf);

    if (cfg.probes) {
        logp("running ARM11 instruction probes (boot.cfg probes=0 skips them)\n");
        log_flush();
        volatile shared_t *sh = SHARED;
        sh->a9_cmd = A9CMD_PROBE;
        u32 t = 0;
        for (; t < 2000 && !sh->probes_done; t++) {
            drain_a11_log();
            delay_ms(1);
        }
        drain_a11_log();
        if (!sh->probes_done) {
            log_err("ARM11 hung in probe %u (encoding %08x). Set probes=0 in boot.cfg.\n",
                    sh->a11_current_probe, sh->probe[sh->a11_current_probe % PROBE_MAX].encoding);
            status(9, CON_RED, "ARM11 HUNG IN PROBE %u", sh->a11_current_probe);
            monitor("ARM11 hung in a probe");
        }
        sh->a9_cmd = A9CMD_NONE;
        report_probes();
    }
    logp("kernel %s @%08x entry %08x\n", cfg.kernel, cfg.kernel_pa, cfg.entry_pa);
    logp("devtree %s @%08x, boot_args @%08x\n", cfg.devtree, cfg.devtree_pa, cfg.bootargs_pa);
    logp("log ring @%08x+%x, topOfKernelData %08x\n", cfg.ring_pa, cfg.ring_size, cfg.top_pa);
    logp("phys %08x virt %08x memSize %08x\n", cfg.phys_base, cfg.virt_base, cfg.mem_size);
    logp("cmdline: \"%s\"\n", cfg.cmdline);
    if (!validate_cfg()) {
        status(9, CON_RED, "boot.cfg INVALID");
        monitor("bad boot.cfg");
    }

    u32 ksize = 0, dsize = 0;
    logp("loading kernel...\n");
    int r = sd_load(cfg.kernel, (void *)cfg.kernel_pa, cfg.devtree_pa - cfg.kernel_pa, &ksize);
    if (r) {
        log_err("kernel load failed (%d)\n", r);
        status(9, CON_RED, "KERNEL LOAD FAILED");
        monitor("kernel load failed");
    }
    log_ok("kernel: %u bytes at %08x\n", ksize, cfg.kernel_pa);
    r = sd_load(cfg.devtree, (void *)cfg.devtree_pa, cfg.bootargs_pa - cfg.devtree_pa, &dsize);
    if (r) {
        log_err("devtree load failed (%d)\n", r);
        monitor("devtree load failed");
    }
    log_ok("devtree: %u bytes at %08x\n", dsize, cfg.devtree_pa);

    if (cfg.shim[0]) {
        u32 ssize = 0;
        r = sd_load(cfg.shim, (void *)cfg.shim_pa, cfg.ring_pa - cfg.shim_pa, &ssize);
        if (r) {
            log_err("shim load failed (%d)\n", r);
            monitor("shim load failed");
        }
        log_ok("shim: %u bytes at %08x (magic %08x)\n", ssize, cfg.shim_pa, *(vu32 *)(cfg.shim_pa + 4));
    }

    if (cfg.rootfs[0]) {
        u32 secs = 0;
        int br = blksrv_open(cfg.rootfs, &secs);
        if (br) {
            log_err("rootfs %s: open failed (%d) - md0 will return I/O errors\n", cfg.rootfs, br);
        } else {
            log_ok("rootfs %s: %u MiB served as md0\n", cfg.rootfs, secs / 2048);
        }
    }

    for (u32 i = 0; i < cfg.nloads; i++) {
        u32 sz = 0, pa = cfg.load_pa[i];
        bool ok_dst = (pa >= 0x1FF80000 && pa < 0x1FFF0000) || in_fcram(pa, 0x1000) ||
                      (pa >= VRAM_BASE && pa < VRAM_BASE + 0x600000);
        if (!ok_dst) {
            log_err("load %s: destination %08x not allowed\n", cfg.load_path[i], pa);
            monitor("bad load= address");
        }
        u32 max = pa < 0x1FFF0000 && pa >= 0x1FF80000 ? 0x1FFF0000 - pa : 0x100000;
        r = sd_load(cfg.load_path[i], (void *)pa, max, &sz);
        if (r) {
            log_err("load %s failed (%d)\n", cfg.load_path[i], r);
            monitor("load= failed");
        }
        log_ok("loaded %s: %u bytes at %08x\n", cfg.load_path[i], sz, pa);
    }

    display_arm(cfg.display_regs);

    u32 entry_word = *(vu32 *)cfg.entry_pa;
    logp("entry word %08x (expect e3a01000 'mov r1,#0')\n", entry_word);

    /* boot_args */
    boot_args_t *ba = (boot_args_t *)cfg.bootargs_pa;
    memset(ba, 0, sizeof(*ba));
    ba->Revision = cfg.ba_revision;
    ba->Version = cfg.ba_version;
    ba->virtBase = cfg.virt_base;
    ba->physBase = cfg.phys_base;
    ba->memSize = cfg.mem_size;
    ba->topOfKernelData = cfg.top_pa;
    ba->Video.v_baseAddr = cfg.video_pa;
    ba->Video.v_display = cfg.video_display;
    ba->Video.v_rowBytes = cfg.video_rowbytes;
    ba->Video.v_width = cfg.video_width;
    ba->Video.v_height = cfg.video_height;
    ba->Video.v_depth = cfg.video_depth;
    ba->machineType = 0;
    ba->deviceTreeP = cfg.devtree_pa - cfg.phys_base + cfg.virt_base;
    ba->deviceTreeLength = dsize;
    strlcpy_(ba->CommandLine, cfg.cmdline, sizeof(ba->CommandLine));
    log_ok("boot_args at %08x: dt va %08x len %u\n", cfg.bootargs_pa, ba->deviceTreeP, dsize);

    /* XNU log ring (filled by the M2 console hook) */
    if (cfg.ring_size > sizeof(xnu_ring_t)) {
        xnu_ring_t *ring = (xnu_ring_t *)cfg.ring_pa;
        memset(ring, 0, cfg.ring_size);
        ring->size = cfg.ring_size - sizeof(xnu_ring_t);
        ring->magic = XNU_RING_MAGIC;
    }
    /* marker pattern in the page-table area: lets the monitor see the entry stub run */
    memset((void *)cfg.top_pa, 0xA5, 0x9000);
    /* Wi-Fi: wifi.cfg, WPA2 key derivation, network mailbox (docs/wifi.md) */
    wifi_init();

    audio_init();                   /* CODEC sound path, before XNU (docs/audio.md) */
    if (!cfg.jump) {
        log_warn("boot.cfg says jump=0: not starting XNU\n");
        monitor("jump disabled");
    }
    for (u32 ms = cfg.countdown_ms; ms; ms = ms > 100 ? ms - 100 : 0) {
        status(10, CON_YELLOW, "jump to XNU in %u.%u s (B = cancel)", ms / 1000, (ms % 1000) / 100);
        if (!(HID_PAD & HID_B)) {
            log_warn("B held: jump cancelled\n");
            status(10, CON_YELLOW, "jump cancelled (B)");
            monitor("jump cancelled");
        }
        delay_ms(100);
    }

    log_color(CON_GREEN, "jumping: ARM11 -> %08x, r0=%08x (boot_args)\n", cfg.entry_pa, cfg.bootargs_pa);
    log_flush();
    volatile shared_t *sh = SHARED;
    sh->jump_pc = cfg.entry_pa;
    sh->jump_r0 = cfg.bootargs_pa;
    sh->a9_cmd = A9CMD_JUMP;
    for (u32 t = 0; t < 1000 && sh->a11_ack != A9CMD_JUMP; t++) delay_ms(1);
    if (sh->a11_ack != A9CMD_JUMP) {
        log_err("ARM11 did not acknowledge the jump command\n");
    } else {
        log_ok("ARM11 acknowledged, now in XNU\n");
    }
    status(10, CON_GREEN, "XNU started");
    monitor("XNU running");
}
