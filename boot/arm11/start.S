@ ios3ds ARM11 (core 0) entry. Luma jumps here in SVC mode with A/I/F masked,
@ MMU + caches off, low vectors (boot11 ROM -> RAM vectors at 0x1FFFFFA0).
@ Core 1 is never touched: it stays parked in boot11.
@ SPDX-License-Identifier: GPL-3.0-or-later

    .arch armv6k
    .fpu vfpv2
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    cpsid aif
    ldr sp, =__svc_stack_top
    cps #0x1B                   @ UND
    ldr sp, =__exc_stack_top
    cps #0x17                   @ ABT
    ldr sp, =__exc_stack_top
    cps #0x13                   @ back to SVC

    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b

    bl a11_main
2:  wfi
    b 2b

@ ---------------------------------------------------------------------------
@ Exception stubs installed in the boot11 RAM vector table during probes.
@ The slot order of the RAM table is not documented; each stub knows its slot
@ and the CPSR mode tells us the exception type. UND -> resume after the
@ instruction (lr_und = insn + 4). ABT -> assume data abort, skip the insn
@ (lr_abt = insn + 8, resume at lr - 4).

    .macro EXC_STUB n
    .global a11_exc_stub\n
a11_exc_stub\n:
    stmfd sp!, {r0-r3, r12, lr}
    mov r0, #\n
    b exc_common
    .endm

    EXC_STUB 0
    EXC_STUB 1
    EXC_STUB 2
    EXC_STUB 3
    EXC_STUB 4
    EXC_STUB 5

exc_common:
    mrs r1, cpsr
    ldr r2, [sp, #20]           @ exception lr
    bl a11_exception            @ returns amount to subtract from lr
    ldr r1, [sp, #20]
    sub r1, r1, r0
    str r1, [sp, #20]
    ldmfd sp!, {r0-r3, r12, pc}^

@ ---------------------------------------------------------------------------
@ Probe functions: r0 preset to a marker; if the instruction traps, the marker
@ survives and the handler reports the trap.
    .macro PROBE name, insn
    .global \name
\name:
    ldr r0, =0xDEADBEEF
    .word \insn
    bx lr
    .endm

    PROBE probe_ppmr,   0xEE1F0F92      @ mrc p15,0,r0,c15,c2,4  ARM1176 peripheral port remap
    PROBE probe_ifar,   0xEE160F50      @ mrc p15,0,r0,c6,c0,2   IFAR
    PROBE probe_pmnc,   0xEE1F0F1C      @ mrc p15,0,r0,c15,c12,0 PMU control
    PROBE probe_didr,   0xEE100E10      @ mrc p14,0,r0,c0,c0,0   debug ID
    PROBE probe_fpsid,  0xEEF00A10      @ vmrs r0, fpsid
    PROBE probe_mvfr0,  0xEEF70A10      @ vmrs r0, mvfr0
    PROBE probe_mvfr1,  0xEEF60A10      @ vmrs r0, mvfr1
    PROBE probe_fpexc,  0xEEF80A10      @ vmrs r0, fpexc
    .ltorg

@ ---------------------------------------------------------------------------
@ CP15 ID register readers
    .macro MRC_FN name, crn, crm, op2
    .global \name
\name:
    mrc p15, 0, r0, \crn, \crm, \op2
    bx lr
    .endm

    MRC_FN rd_midr,  c0, c0, 0
    MRC_FN rd_ctr,   c0, c0, 1
    MRC_FN rd_tlbtr, c0, c0, 3
    MRC_FN rd_mpidr, c0, c0, 5
    MRC_FN rd_pfr0,  c0, c1, 0
    MRC_FN rd_pfr1,  c0, c1, 1
    MRC_FN rd_dfr0,  c0, c1, 2
    MRC_FN rd_afr0,  c0, c1, 3
    MRC_FN rd_mmfr0, c0, c1, 4
    MRC_FN rd_mmfr1, c0, c1, 5
    MRC_FN rd_mmfr2, c0, c1, 6
    MRC_FN rd_mmfr3, c0, c1, 7
    MRC_FN rd_isar0, c0, c2, 0
    MRC_FN rd_isar1, c0, c2, 1
    MRC_FN rd_isar2, c0, c2, 2
    MRC_FN rd_isar3, c0, c2, 3
    MRC_FN rd_isar4, c0, c2, 4
    MRC_FN rd_isar5, c0, c2, 5
    MRC_FN rd_sctlr, c1, c0, 0
    MRC_FN rd_actlr, c1, c0, 1
    MRC_FN rd_cpacr, c1, c0, 2

    .global rd_cpsr
rd_cpsr:
    mrs r0, cpsr
    bx lr

@ enable VFP: CPACR cp10/cp11 full access, flush prefetch, FPEXC.EN
    .global a11_enable_vfp
a11_enable_vfp:
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #0xF00000
    mcr p15, 0, r0, c1, c0, 2
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 4
    mov r0, #0x40000000
    .word 0xEEE80A10                @ vmsr fpexc, r0
    mov r0, #0x03000000             @ RunFast: flush-to-zero + default NaN
    .word 0xEEE10A10                @ vmsr fpscr, r0
    bx lr

@ void a11_jump(u32 pc, u32 r0) - never returns.
@ Caches/MMU are still off (never enabled), but invalidate everything anyway.
    .global a11_jump
a11_jump:
    cpsid aif
    mov r4, r0
    mov r5, r1
    mov r0, #0
    mcr p15, 0, r0, c7, c7, 0       @ invalidate I+D caches
    mcr p15, 0, r0, c8, c7, 0       @ invalidate TLBs
    mcr p15, 0, r0, c7, c10, 4      @ DSB
    mcr p15, 0, r0, c7, c5, 4       @ prefetch flush
    mov r0, r5
    mov r1, #0
    mov r2, #0
    mov r3, #0
    bx r4
