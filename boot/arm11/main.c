/*
 * ios3ds ARM11 stub (M1): identify the CPU, probe the CP15/VFP encodings the
 * iOS 4.2.1 kernel uses, enable VFP, then wait for the ARM9 to say "jump".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "util.h"

u32 rd_midr(void), rd_ctr(void), rd_tlbtr(void), rd_mpidr(void);
u32 rd_pfr0(void), rd_pfr1(void), rd_dfr0(void), rd_afr0(void);
u32 rd_mmfr0(void), rd_mmfr1(void), rd_mmfr2(void), rd_mmfr3(void);
u32 rd_isar0(void), rd_isar1(void), rd_isar2(void), rd_isar3(void), rd_isar4(void), rd_isar5(void);
u32 rd_sctlr(void), rd_actlr(void), rd_cpacr(void), rd_cpsr(void);
u32 probe_ppmr(void), probe_ifar(void), probe_pmnc(void), probe_didr(void);
u32 probe_fpsid(void), probe_mvfr0(void), probe_mvfr1(void), probe_fpexc(void);
void a11_enable_vfp(void);
void a11_jump(u32 pc, u32 r0) __attribute__((noreturn));
extern char a11_exc_stub0[], a11_exc_stub1[], a11_exc_stub2[], a11_exc_stub3[], a11_exc_stub4[], a11_exc_stub5[];

static volatile shared_t *const sh = SHARED;

static volatile int in_probe;
static volatile int probe_trapped;
static volatile u32 trap_slot, trap_mode;

/* ---- tiny log to the ARM9 (ring in the mailbox) ---- */
static void a11_puts(const char *s)
{
    while (*s) {
        u32 h = sh->log_head;
        sh->log_buf[h % SHARED_LOG_SIZE] = *s++;
        sh->log_head = h + 1;
    }
}

static void a11_printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    a11_puts(buf);
}

/* called from exc_common (asm) in UND/ABT mode; returns lr adjustment */
u32 a11_exception(u32 slot, u32 cpsr, u32 lr)
{
    u32 mode = cpsr & 0x1F;
    if (in_probe) {
        probe_trapped = 1;
        trap_slot = slot;
        trap_mode = mode;
        return mode == 0x17 ? 4 : 0;   /* ABT: skip faulting insn; UND: lr already past it */
    }
    sh->unexpected_count++;
    sh->unexpected_slot = slot;
    sh->unexpected_mode = mode;
    sh->unexpected_lr = lr;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static u32 saved_vectors[12];

static void install_probe_vectors(void)
{
    vu32 *v = (vu32 *)A11_RAM_VECTORS;
    char *stubs[6] = { a11_exc_stub0, a11_exc_stub1, a11_exc_stub2,
                       a11_exc_stub3, a11_exc_stub4, a11_exc_stub5 };
    for (int i = 0; i < 12; i++) saved_vectors[i] = v[i];
    for (int i = 0; i < 6; i++) {
        v[2 * i] = 0xE51FF004;           /* ldr pc, [pc, #-4] */
        v[2 * i + 1] = (u32)stubs[i];
    }
}

static void restore_vectors(void)
{
    vu32 *v = (vu32 *)A11_RAM_VECTORS;
    for (int i = 0; i < 12; i++) v[i] = saved_vectors[i];
}

static void run_probe(u32 idx, u32 encoding, u32 (*fn)(void))
{
    volatile probe_t *p = &sh->probe[idx];
    sh->a11_current_probe = idx;
    p->encoding = encoding;
    p->result = PROBE_NOT_RUN;
    probe_trapped = 0;
    in_probe = 1;
    u32 v = fn();
    in_probe = 0;
    p->value = v;
    if (probe_trapped) {
        p->result = PROBE_EXCEPTION;
        p->exc_slot = trap_slot;
        p->exc_mode = trap_mode;
    } else {
        p->result = PROBE_OK;
    }
    if (idx + 1 > sh->nprobes) sh->nprobes = idx + 1;
}


/* ---- New 3DS: model, 804 MHz, L2 cache, timer calibration (docs/n3ds.md) ----
   The clock switch is libn3ds PDN_core123Init()/PDN_setSocmode() and
   linux-3ds firm_linux_loader smp.c: GIC interrupt 88 enabled for core 0,
   PDN_LGR_CNT = L2C | FCRAM/QTM extension, ~400 cycles, write the mode, WFI
   until PDN_LGR_SOCMODE bit 15 (ack), write it back, clear GIC 88; then
   CFG11_GPU_N3DS_CNT like libn3ds. Here MMU and caches are still off. */
#define CFG11_SOCINFO   (*(vu16 *)0x10140FFCu)
#define CFG11_GPU_N3DS  (*(vu8 *)0x10140400u)
#define PDN_SOCMODE     (*(vu16 *)0x10141300u)
#define PDN_LGR_CNT     (*(vu16 *)0x10141304u)
#define MPC(off)        (*(vu32 *)(0x17E00000u + (off)))
#define MPC8(off)       (*(vu8 *)(0x17E00000u + (off)))
#define L2C(off)        (*(vu32 *)(0x17E10000u + (off)))
#define GICD_CLR_EN(n)  MPC(0x1180u + 4u * (n))
#define GICD_SET_EN(n)  MPC(0x1100u + 4u * (n))
#define GICD_CLR_PEND(n) MPC(0x1280u + 4u * (n))
#define IRQ_SOCMODE     88u
#define PERIPH_O3DS     134055928u      /* 268111856 / 2 */

static void wait_cycles(u32 n) { while (n--) __asm__ volatile("nop"); }

/* returns 1 when acknowledged; the private timer (GIC 29, ~10 ms per wake at
   any clock) bounds the WFI loop so a missing ack cannot hang the boot */
static int set_socmode(u32 mode)
{
    u32 wakes = 0;
    PDN_SOCMODE = (u16)mode;
    MPC(0x608) = 0; MPC(0x60C) = 1;
    MPC(0x600) = 0x400000u;             /* 4 Mi PERIPHCLK ticks: 31 ms at 134 MHz, 10 ms at 402 */
    MPC(0x608) = 4u | 2u | 1u;          /* IT enable, auto-reload, enable, prescaler 0 */
    GICD_CLR_PEND(0) = 1u << 29;
    GICD_SET_EN(0) = 1u << 29;
    while (!(PDN_SOCMODE & 0x8000u)) {
        __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0) : "memory");   /* WFI */
        if (MPC(0x60C) & 1u) {
            MPC(0x60C) = 1;
            GICD_CLR_PEND(0) = 1u << 29;
            if (++wakes > 100) break;   /* > 1 s */
        }
    }
    MPC(0x608) = 0; MPC(0x60C) = 1;
    GICD_CLR_EN(0) = 1u << 29;
    GICD_CLR_PEND(0) = 1u << 29;
    if (!(PDN_SOCMODE & 0x8000u)) return 0;
    PDN_SOCMODE = PDN_SOCMODE;          /* acknowledge (bit 15 set) */
    GICD_CLR_PEND(IRQ_SOCMODE / 32) = 1u << (IRQ_SOCMODE % 32);
    return 1;
}

/* MPCore PERIPHCLK against the ARM9's 65457 Hz timer: the ARM9 marks the
   start (cal_state 2) and, 1309 of its ticks (20.0 ms) later, the end (3).
   0 = the ARM9 did not take part. */
static u32 measure_periphclk(void)
{
    u32 w0, w1, t;
    MPC(0x634) = 0x12345678u; MPC(0x634) = 0x87654321u;    /* watchdog -> timer mode */
    MPC(0x620) = 0xFFFFFFFFu;
    MPC(0x628) = 2u | 1u;                                   /* prescaler 0, auto-reload, on */
    sh->cal_state = 1;
    for (t = 0; sh->cal_state != 2; t++) if (t > 50000000u) goto fail;
    w0 = MPC(0x624);
    for (t = 0; sh->cal_state != 3; t++) if (t > 50000000u) goto fail;
    w1 = MPC(0x624);
    MPC(0x628) = 0;
    sh->cal_a11 = w0 - w1;
    sh->cal_state = 4;
    return (u32)((u64)(w0 - w1) * 65457u / 1309u);
fail:
    MPC(0x628) = 0;
    sh->cal_state = 5;
    return 0;
}

static void l2_enable(void)
{
    u32 aux, ways;
    sh->n3ds_l2_id = L2C(0x000);
    if (L2C(0x100) & 1u) { a11_printf("[a11] L2C-310 already on: left alone\n"); return; }
    aux = L2C(0x104) | (1u << 22);      /* shared attribute override (no "cacheable no-allocate" for shared NC) */
    L2C(0x104) = aux;
    L2C(0xF40) = 2u;                    /* debug control DWB: force write-through, never dirty */
    if ((L2C(0xF40) & 2u) == 0) { a11_printf("[a11] L2C-310: write-through refused, L2 stays off\n"); return; }
    ways = (aux & (1u << 16)) ? 0xFFFFu : 0xFFu;
    L2C(0x77C) = ways;                  /* invalidate all ways */
    while (L2C(0x77C) & ways) ;
    L2C(0x730) = 0;                     /* sync */
    while (L2C(0x730) & 1u) ;
    L2C(0x220) = 0x1FFu;                /* clear interrupts */
    L2C(0x100) = 1u;
    sh->n3ds_l2_aux = L2C(0x104);
    sh->n3ds_l2_ctrl = L2C(0x100);
    if (sh->n3ds_l2_ctrl & 1u) sh->n3ds_res |= N3RES_L2;
}

static void a11_n3ds(void)
{
    u32 req = sh->n3ds_req, res = 0, soc = CFG11_SOCINFO, hz, r, pre, shift = 0;
    sh->n3ds_socinfo = soc;
    sh->n3ds_res = 0;
    if (soc & 2u) {
        res |= N3RES_NEW;
        MPC(0x100) = 1;                                     /* GIC CPU interface on, all priorities */
        MPC(0x104) = 0xF0;
        MPC(0x1000) |= 1u;
        for (u32 i = 0; i < 4; i++) GICD_CLR_EN(i) = 0xFFFFFFFFu;
        GICD_CLR_PEND(IRQ_SOCMODE / 32) = 1u << (IRQ_SOCMODE % 32);
        MPC8(0x1400 + IRQ_SOCMODE) = 0;
        MPC8(0x1800 + IRQ_SOCMODE) = 1;
        GICD_SET_EN(IRQ_SOCMODE / 32) = 1u << (IRQ_SOCMODE % 32);
        PDN_SOCMODE = PDN_SOCMODE;                          /* stale ack from a bootloader */
        if (req & N3REQ_CLOCK) {
            PDN_LGR_CNT = (soc & 4u) ? 0x101u : 0x001u;     /* L2C (LGR2) + extension RAM */
            wait_cycles(403);
        }
        if ((req & N3REQ_CLOCK) && (PDN_SOCMODE & 7u) != 5u) {
            if (!set_socmode((soc & 4u) ? 5u : 3u)) res |= N3RES_TIMEOUT;
            else CFG11_GPU_N3DS = 3;                        /* TEX_FIX | N3DS_MODE */
        } else if (!(req & N3REQ_CLOCK) && (PDN_SOCMODE & 7u) != 0u) {
            if (!set_socmode(0)) res |= N3RES_TIMEOUT;      /* fallback: plain CTR mode like libn3ds */
        }
        GICD_CLR_EN(IRQ_SOCMODE / 32) = 1u << (IRQ_SOCMODE % 32);
        if ((PDN_SOCMODE & 7u) == 5u) res |= N3RES_804;
        sh->n3ds_socmode = PDN_SOCMODE;
        sh->n3ds_lgr_cnt = PDN_LGR_CNT;
    }
    /* timers: XNU's timebase is 1047311 Hz = PERIPHCLK / 128 on the Old 3DS.
       PERIPHCLK x3 (804 MHz) needs /384 > the 8-bit prescaler: /192 and the
       shim halves the counts (tbshift 1). */
    hz = measure_periphclk();
    sh->n3ds_periph_hz = hz;
    r = hz ? (hz + PERIPH_O3DS / 2) / PERIPH_O3DS : ((res & N3RES_804) ? 3u : 1u);
    if (r != 1 && r != 2 && r != 3) r = (res & N3RES_804) ? 3u : 1u;
    pre = r == 1 ? 127u : r == 2 ? 255u : 191u;
    if (r == 3) shift = 1;
    sh->n3ds_res = res;
    if ((req & N3REQ_L2) && (res & N3RES_804) && (soc & 4u)) l2_enable();   /* L2 only with the LGR2 clock mode */
    a11_printf("[a11] %s 3DS: SOCINFO %x, SOCMODE %x, LGR_CNT %x, PERIPHCLK %u Hz, timer /%u >>%u%s\n",
               (res & N3RES_NEW) ? "New" : "Old", soc, sh->n3ds_socmode, sh->n3ds_lgr_cnt, hz,
               pre + 1, shift, (res & N3RES_TIMEOUT) ? ", CLOCK SWITCH TIMED OUT" : "");
    sh->n3ds_res = sh->n3ds_res | pre | (shift << 8) | N3RES_DONE;
}

void a11_main(void)
{
    /* own the mailbox: clear everything except the ARM9's token */
    u32 token = sh->a9_token;
    for (u32 i = 0; i < sizeof(shared_t) / 4; i++) ((vu32 *)sh)[i] = 0;
    sh->a9_token = token;
    sh->struct_size = sizeof(shared_t);
    sh->a11_stage = A11_STAGE_ENTRY;
    a11_printf("[a11] ios3ds %s ARM11 stub at %p\n", IOS3DS_VERSION, a11_main);

    sh->midr = rd_midr();
    sh->ctr = rd_ctr();
    sh->tlbtr = rd_tlbtr();
    sh->mpidr = rd_mpidr();
    sh->id_pfr0 = rd_pfr0();
    sh->id_pfr1 = rd_pfr1();
    sh->id_dfr0 = rd_dfr0();
    sh->id_afr0 = rd_afr0();
    sh->id_mmfr[0] = rd_mmfr0();
    sh->id_mmfr[1] = rd_mmfr1();
    sh->id_mmfr[2] = rd_mmfr2();
    sh->id_mmfr[3] = rd_mmfr3();
    sh->id_isar[0] = rd_isar0();
    sh->id_isar[1] = rd_isar1();
    sh->id_isar[2] = rd_isar2();
    sh->id_isar[3] = rd_isar3();
    sh->id_isar[4] = rd_isar4();
    sh->id_isar[5] = rd_isar5();
    sh->sctlr = rd_sctlr();
    sh->actlr = rd_actlr();
    sh->cpsr = rd_cpsr();
    sh->a11_stage = A11_STAGE_IDREGS;

    sh->a11_stage = A11_STAGE_VFP;
    a11_enable_vfp();
    sh->cpacr = rd_cpacr();
    a11_printf("[a11] VFP enabled, CPACR %08x\n", sh->cpacr);

    sh->magic = SHARED_MAGIC;
    sh->a11_stage = A11_STAGE_READY;
    for (u32 hb = 0;; hb++) {
        sh->a11_heartbeat = hb;
        sh->a11_echo = sh->a9_token;
        if (sh->a9_cmd == A9CMD_PROBE && !sh->probes_done) {
            /* Encodings whose behaviour on MPCore the TRM calls UNPREDICTABLE.
               Run on request only, so a hang here can be bypassed (probes=0). */
            sh->a11_stage = A11_STAGE_PROBES;
            install_probe_vectors();
            run_probe(0, 0xEE1F0F92, probe_ppmr);
            run_probe(1, 0xEE160F50, probe_ifar);
            run_probe(2, 0xEE1F0F1C, probe_pmnc);
            run_probe(3, 0xEE100E10, probe_didr);
            run_probe(4, 0xEEF00A10, probe_fpsid);
            run_probe(5, 0xEEF70A10, probe_mvfr0);
            run_probe(6, 0xEEF60A10, probe_mvfr1);
            run_probe(7, 0xEEF80A10, probe_fpexc);
            restore_vectors();
            a11_printf("[a11] probes done\n");
            sh->probes_done = 1;
            sh->a11_stage = A11_STAGE_READY;
        }
        if (sh->a9_cmd == A9CMD_N3DS && !(sh->n3ds_res & N3RES_DONE)) {
            sh->a11_stage = A11_STAGE_N3DS;
            a11_n3ds();
            sh->a11_stage = A11_STAGE_READY;
        }
        if (sh->a9_cmd == A9CMD_JUMP) {
            u32 pc = sh->jump_pc, r0 = sh->jump_r0;
            /* MPCore coherency like libn3ds mmu.c / start.s: reset the SCU,
               invalidate its tag RAMs, enable it, then ACTLR.SMP (bit 5).
               Luma hands over with ACTLR 0xF (AMP): there, exclusive
               accesses (LDREX/STREX, i.e. every XNU lock and atomic) to
               memory the page tables mark Shared never succeed on this bus,
               so XNU spun silently right after "iBoot version" on hardware
               (QEMU models no caches or monitors). XNU's own ACTLR writes
               are patched out (kernel/patches.json actlr-write), so it stays. */
            vu32 *scu = (vu32 *)0x17E00000u;
            u32 actlr;
            scu[0] = 0x1FFEu;               /* SCU off, all alias/access bits */
            scu[3] = 0xFFFFu;               /* invalidate all tag RAM ways */
            scu[0] = 0x1FFFu;               /* SCU on */
            __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
            actlr |= 0x2Fu;                 /* SMP + return stack + branch prediction + folding */
            __asm__ volatile("mcr p15, 0, %0, c1, c0, 1\n"
                             "mcr p15, 0, %1, c7, c7, 0\n"     /* invalidate both caches */
                             "mcr p15, 0, %1, c7, c10, 4\n"    /* DSB */
                             "mcr p15, 0, %1, c7, c5, 4"        /* flush prefetch buffer */
                             : : "r"(actlr), "r"(0) : "memory");
            __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
            a11_printf("[a11] SCU on, SMP mode: ACTLR %08x\n", actlr);
            a11_printf("[a11] jumping to %08x r0=%08x\n", pc, r0);
            sh->a11_stage = A11_STAGE_JUMPING;
            sh->a11_ack = A9CMD_JUMP;
            a11_jump(pc, r0);
        }
    }
}
