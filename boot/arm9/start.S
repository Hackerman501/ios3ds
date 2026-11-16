@ ios3ds ARM9 entry (called by Luma3DS / boot9strap chainloader)
@ r0 = argc, r1 = argv (argv lives on the loader's ITCM stack - copied in main)
@ CPU state: SVC, IRQ/FIQ masked, MPU + caches off (we keep them off: every
@ buffer the ARM11 also touches stays coherent that way).
@ SPDX-License-Identifier: GPL-3.0-or-later

    .arm
    .section .text.start, "ax"
    .global _start
_start:
    msr cpsr_c, #0xD3           @ SVC, IRQ+FIQ off
    ldr sp, =__stack_top
    mov r4, r0
    mov r5, r1

    @ make sure MPU and caches are off whatever the loader did
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #(1 << 0)       @ MPU
    bic r0, r0, #(1 << 2)       @ D-cache
    bic r0, r0, #(1 << 12)      @ I-cache
    mcr p15, 0, r0, c1, c0, 0

    @ clear .bss
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b

    mov r0, r4
    mov r1, r5
    bl a9_main
2:  b 2b

    .global waitcycles
    .type waitcycles, %function
@ void waitcycles(u32 n): ~4 cycles per iteration
waitcycles:
    subs r0, r0, #1
    bne waitcycles
    bx lr

    .global a9_get_cpsr
a9_get_cpsr:
    mrs r0, cpsr
    bx lr

    .ltorg
