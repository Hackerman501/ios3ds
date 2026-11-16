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
        if (sh->a9_cmd == A9CMD_JUMP) {
            u32 pc = sh->jump_pc, r0 = sh->jump_r0;
            a11_printf("[a11] jumping to %08x r0=%08x\n", pc, r0);
            sh->a11_stage = A11_STAGE_JUMPING;
            sh->a11_ack = A9CMD_JUMP;
            a11_jump(pc, r0);
        }
    }
}
