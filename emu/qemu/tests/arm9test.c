/*
 * ios3ds emulator self-test, ARM9 side: pokes the ARM9-only devices of the
 * `nintendo3ds` QEMU machine (CFG9, TMIO1, IRQ controller, timers) and leaves
 * the observed values in FCRAM at 0x20001000 for emu/qemu/tests/iotest.py.
 * Built and run by iotest.py; not part of the payload.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
typedef volatile unsigned char vu8;
typedef volatile unsigned short vu16;
typedef volatile unsigned int vu32;
typedef unsigned int u32;

__asm__(".section .text.start\n"
        ".global _start\n"
        "_start:\n"
        "    ldr sp, =0x08100000\n"
        "    bl main\n"
        "1:  b 1b\n"
        ".ltorg\n"
        ".text\n");

#define R            ((vu32 *)0x20001000)
#define SYSPROT      (*(vu16 *)0x10000000)
#define SDMMCCTL     (*(vu16 *)0x10000020)
#define IRQ_IE       (*(vu32 *)0x10001000)
#define IRQ_IF       (*(vu32 *)0x10001004)
#define TMR_VAL(n)   (*(vu16 *)(0x10003000 + 4 * (n)))
#define TMR_CNT(n)   (*(vu16 *)(0x10003002 + 4 * (n)))
#define TMIO(o)      (*(vu16 *)(0x10006000 + (o)))

static volatile u32 irq_count;

static void __attribute__((interrupt("IRQ"))) irq_handler(void)
{
    irq_count++;
    IRQ_IF = IRQ_IF;        /* acknowledge everything pending */
}

static u32 tmio_status(void)
{
    return TMIO(0x1C) | (u32)TMIO(0x1E) << 16;
}

static u32 tmio_cmd(u32 cmd, u32 arg)
{
    TMIO(0x1C) = 0;
    TMIO(0x1E) = 0;
    TMIO(0x04) = arg;
    TMIO(0x06) = arg >> 16;
    TMIO(0x00) = cmd;
    for (int i = 0; i < 100000 && !(TMIO(0x1C) & 1); i++) {
    }
    return tmio_status();
}

static void spin(u32 n)
{
    for (volatile u32 i = 0; i < n; i++) {
    }
}

void main(void)
{
    R[0] = 0x54455354;                  /* 'TEST' */
    R[1] = SDMMCCTL;                    /* Luma handoff: 0x0200 */
    R[2] = tmio_status();               /* idle with a card: 0x208004A0 */
    R[3] = SYSPROT;                     /* 0x0103 */

    TMIO(0xE0) = 0;                     /* soft reset */
    TMIO(0xE0) = 1;
    TMIO(0x100) = 0xFFFF;               /* FIFO32 status bits must not stick */
    R[4] = TMIO(0x100);                 /* 0x1802 */
    TMIO(0x100) = 0x0002;

    TMIO(0x02) = 0;
    TMIO(0x24) = 0x0020;                /* clock stopped */
    R[5] = tmio_cmd(0x0000, 0);         /* timeout + RESP_END: 0x20C004A1 */
    R[17] = TMIO(0x2C) | (u32)TMIO(0x2E) << 16;   /* ERR_CMD_RESP_TMOUT: 0x00010000 */
    TMIO(0x24) = 0x0120;
    TMIO(0x02) = 1;                     /* port 1 = eMMC, not there */
    R[6] = tmio_cmd(0x0000, 0);         /* 0x20C00001 */
    TMIO(0x02) = 0;
    R[7] = tmio_cmd(0x0000, 0);         /* CMD0: 0x208004A1 */
    R[8] = tmio_cmd(0x0408, 0x1AA);     /* CMD8 R7: 0x208004A1 */
    R[9] = TMIO(0x0C) | (u32)TMIO(0x0E) << 16;    /* 0x000001AA */

    SDMMCCTL = 0x0201;                  /* slot power off */
    R[10] = tmio_status() & ~1u;        /* DAT3 detect gone: 0x208000A0 */
    R[11] = tmio_cmd(0x0408, 0x1AA);    /* 0x20C000A1 */
    SDMMCCTL = 0x0200;

    /* TMIO IRQ line -> IRQ_IF bit 16 once RESP_END is unmasked */
    IRQ_IF = 0xFFFFFFFF;
    R[12] = IRQ_IF;                     /* 0 */
    TMIO(0x20) = 0xFFFE;
    tmio_cmd(0x0000, 0);
    R[13] = IRQ_IF;                     /* 0x00010000 */
    TMIO(0x20) = 0xFFFF;
    TMIO(0x1C) = 0;
    IRQ_IF = 0xFFFFFFFF;

    /* timers */
    TMR_VAL(0) = 0;
    TMR_CNT(0) = 0x83;                  /* enable, /1024 */
    spin(20000);
    R[14] = TMR_VAL(0);                 /* > 0 */
    R[15] = TMR_CNT(0);                 /* 0x83 */
    TMR_CNT(0) = 0;

    TMR_VAL(1) = 0xFF00;                /* overflows every 256 ticks */
    TMR_CNT(1) = 0xC0;                  /* enable + IRQ */
    TMR_VAL(2) = 0;
    TMR_CNT(2) = 0x84;                  /* count-up on timer 1 overflows */
    for (int i = 0; i < 1000000 && !(IRQ_IF & (1u << 9)); i++) {
    }
    R[16] = IRQ_IF;                     /* 0x00000200 */
    spin(20000);
    R[18] = TMR_VAL(2);                 /* > 0: cascade counts */
    R[19] = TMR_VAL(1);                 /* >= 0xFF00: reload honoured */

    /* IRQ delivery through boot9's vector -> RAM veneer at 0x08000000 */
    *(vu32 *)0x08000000 = 0xE51FF004;   /* ldr pc, [pc, #-4] */
    *(vu32 *)0x08000004 = (u32)irq_handler;
    IRQ_IF = 0xFFFFFFFF;
    IRQ_IE = 1u << 9;
    __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0");
    spin(50000);
    __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0");
    IRQ_IE = 0;
    TMR_CNT(1) = 0;
    TMR_CNT(2) = 0;
    R[20] = irq_count;                  /* > 0 */
    R[21] = 0x454E4F44;                 /* 'DONE' */
}
