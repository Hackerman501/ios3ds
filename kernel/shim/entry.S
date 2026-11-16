@ ios3ds kernel shim: ARMv6 code the patched 4.2.1 kernel branches into.
@ Linked at VA 0x80840000 (PA 0x20840000, below topOfKernelData, so XNU maps
@ it - RWX, physmap - and never reuses it). See kernel/shim/shim.h for the
@ memory layout shared with the ARM9 (mailbox, bounce buffer, log ring).
@
@ Entry points sit at fixed offsets (kernel/patches.json: @shim+0xNN@):
@   +0x00  b shim_putc            serial "transmit data"     (r0 = char)
@   +0x10  b shim_get_abstime     cpu_data timer hook +0x70  (r3 = cpu_data)
@   +0x14  b shim_get_decrementer cpu_data timer hook +0x64  (r3 = cpu_data)
@   +0x18  b shim_set_decrementer cpu_data timer hook +0x68  (r0 = ticks, r3 = cpu_data)
@   +0x1C  b shim_irq             kernel IRQ handler entry (IRQ mode)
@   +0x20  b shim_mdevstrategy    bsd/dev/memdev.c mdevstrategy(buf_t) -> SD via ARM9
@
@ Time base: MPCore watchdog in timer mode = free-running 32-bit down-counter,
@ extended to 64 bits here. Deadlines: MPCore private timer, one-shot, GIC
@ IRQ 29. Both run at PERIPHCLK/(PRESCALE+1) = 134055928/128 = 1047311 Hz,
@ which is what kernel/dtgen.py puts in timebase-frequency.
@ The kernel takes its timer interrupt as an FIQ (handler address in r9_fiq);
@ the GIC can only raise IRQ, so shim_irq re-enters the kernel's FIQ handler
@ as if a real FIQ had been taken.
@
@ SPDX-License-Identifier: GPL-3.0-or-later

    .arch armv6k
    .arm
    .section .text, "ax"

    .equ SHIM_SIZE,     0x10000         @ ring header follows the shim region
    .equ RING_SIZE_OFF, 4
    .equ RING_HEAD_OFF, 8
    .equ RING_BUF_OFF,  16

    .equ PHYS_BASE,     0x20000000
    .equ VIRT_BASE,     0x80000000
    .equ PRIV_PA,       0x17E00000      @ MPCore private region (SCU/GIC/timers)
    .equ PRIV_VA,       0xBFF00000      @ 1 MiB section we add to the kernel L1
    .equ PRIV_SECTION,  (PRIV_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)  @ AP=01 XN B (device), section

    .equ GICC_CTRL, 0x100
    .equ GICC_PMR,  0x104
    .equ GICC_IAR,  0x10C
    .equ GICC_EOIR, 0x110
    .equ GICD_CTRL, 0x1000
    .equ GICD_ISENABLER0, 0x1100
    .equ GICD_IPRIORITY,  0x1400
    .equ PT_LOAD,   0x600
    .equ PT_COUNT,  0x604
    .equ PT_CTRL,   0x608
    .equ PT_ISR,    0x60C
    .equ WD_LOAD,   0x620
    .equ WD_COUNT,  0x624
    .equ WD_CTRL,   0x628
    .equ WD_DISABLE,0x634

    .equ PRESCALE,  127
    .equ PT_CTRL_RUN, ((PRESCALE << 8) | 4 | 1)     @ IT enable + enable, one-shot
    .equ WD_CTRL_RUN, ((PRESCALE << 8) | 2 | 1)     @ auto-reload + enable, timer mode
    .equ IRQ_PTIMER, 29
    .equ IRQ_PDC0,   42                             @ top screen VBlank
    .equ PDC_PA,     0x10400000                     @ GPU/PDC block (1 MiB section)
    .equ PDC_VA,     0xBFE00000
    .equ PDC0,       0x400
    .equ AXI_PA,     0x1FF00000                     @ DSP RAM + AXI WRAM (fake S5L register pages)
    .equ AXI_VA,     0xBFD00000
    .equ KERNEL_IRQ_CONT, 0x80066494                @ kernel IRQ handler after the 2 patched insns

    .global _shim_base
_shim_base:
    b shim_putc                     @ +0x00
    .word 0x4D494853                @ +0x04 'SHIM'
    .word 2                         @ +0x08 version
    .word 0                         @ +0x0C
    b shim_get_abstime              @ +0x10
    b shim_get_decrementer          @ +0x14
    b shim_set_decrementer          @ +0x18
    b shim_irq                      @ +0x1C
    b shim_mdevstrategy             @ +0x20 (C, blk.c)

@ ---------------------------------------------------------------------------
@ statistics / state (read by the ARM9 via physical address = shim_pa + off)
    .balign 32
st_inited:      .word 0             @ +0x40 
st_ticks_hi:    .word 0             @ +0x44 high word of the 64-bit counter
st_last_up:     .word 0             @ +0x48 
st_timer_irqs:  .word 0             @ +0x4C 
st_other_irqs:  .word 0             @ +0x50 
st_last_irq:    .word 0             @ +0x54 
st_deferred:    .word 0             @ +0x58 
st_l1_old:      .word 0             @ +0x5C L1 entry found at PRIV_VA before we took it
irq_save0:      .word 0             @ +0x60 
irq_save1:      .word 0             @ +0x64 
irq_save2:      .word 0             @ +0x68 
st_panic_req:   .word 0             @ +0x6C ARM9 sets 1: panic (dumps all thread backtraces)
cfg_vic0_pa:    .word 0             @ +0x70 fake VIC0 page PA (patched in by mkboot)
cfg_clcd_pa:    .word 0             @ +0x74 fake CLCD page PA (patched in by mkboot)
st_vblanks:     .word 0             @ +0x78
st_injected:    .word 0             @ +0x7C

@ ---------------------------------------------------------------------------
@ void shim_putc(int c): append to the log ring; clean the touched lines so
@ the ARM9 (reads DRAM, uncached) sees them. Clobbers r0-r3, r12.
shim_putc:
    adr r1, _shim_base
    add r1, r1, #SHIM_SIZE          @ r1 = ring header (VA)
    ldr r2, [r1, #RING_HEAD_OFF]
    ldr r3, [r1, #RING_SIZE_OFF]
    cmp r3, #0
    bxeq lr
    sub r3, r3, #1
    and r3, r2, r3
    add r3, r3, r1
    add r3, r3, #RING_BUF_OFF
    strb r0, [r3]
    bic r3, r3, #31
    mcr p15, 0, r3, c7, c10, 1
    add r2, r2, #1
    str r2, [r1, #RING_HEAD_OFF]
    add r3, r1, #RING_HEAD_OFF
    bic r3, r3, #31
    mcr p15, 0, r3, c7, c10, 1
    mov r3, #0
    mcr p15, 0, r3, c7, c10, 4
    bx lr

@ ---------------------------------------------------------------------------
@ shim_init: map the MPCore private region at PRIV_VA, start the free-running
@ watchdog counter, enable the GIC for the private timer. Idempotent.
@ Clobbers r0-r2, r12. Must be called with interrupts in any state.
shim_init:
    ldr r0, st_inited
    cmp r0, #0
    bxne lr
    mrs r12, cpsr
    cpsid if
    @ L1 table VA = (TTBR1 & ~0x3FFF) - PHYS_BASE + VIRT_BASE
    mrc p15, 0, r0, c2, c0, 1
    bic r0, r0, #0xFF
    bic r0, r0, #0x3F00
    sub r0, r0, #PHYS_BASE
    add r0, r0, #VIRT_BASE
    ldr r1, =((PRIV_VA >> 20) << 2)
    add r0, r0, r1
    ldr r1, [r0]
    str r1, st_l1_old
    ldr r1, =PRIV_SECTION
    str r1, [r0]
    mcr p15, 0, r0, c7, c10, 1      @ clean the L1 entry (walks don't snoop L1)
    mov r1, #0
    mcr p15, 0, r1, c7, c10, 4      @ DSB
    ldr r0, =PRIV_VA
    mcr p15, 0, r0, c8, c7, 1       @ TLB invalidate by MVA
    mcr p15, 0, r1, c7, c10, 4
    mcr p15, 0, r1, c7, c5, 4       @ prefetch flush

    @ two more 1 MiB device sections: PDC (VBlank) and AXI WRAM (fake regs)
    mrc p15, 0, r2, c2, c0, 1
    bic r2, r2, #0xFF
    bic r2, r2, #0x3F00
    sub r2, r2, #PHYS_BASE
    add r2, r2, #VIRT_BASE
    ldr r1, =((PDC_VA >> 20) << 2)
    add r1, r1, r2
    ldr r0, =(PDC_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)
    str r0, [r1]
    mcr p15, 0, r1, c7, c10, 1
    ldr r1, =((AXI_VA >> 20) << 2)
    add r1, r1, r2
    ldr r0, =(AXI_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)
    str r0, [r1]
    mcr p15, 0, r1, c7, c10, 1
    mov r1, #0
    mcr p15, 0, r1, c7, c10, 4
    ldr r0, =PDC_VA
    mcr p15, 0, r0, c8, c7, 1
    ldr r0, =AXI_VA
    mcr p15, 0, r0, c8, c7, 1
    mcr p15, 0, r1, c7, c10, 4
    mcr p15, 0, r1, c7, c5, 4
    @ PDC0: VBlank IRQ on (H and error IRQs off), ack anything pending
    ldr r2, =(PDC_VA + PDC0)
    ldr r1, [r2, #0x74]
    bic r1, r1, #(1 << 9)
    orr r1, r1, #((1 << 8) | (1 << 10))
    str r1, [r2, #0x74]
    ldr r1, [r2, #0x78]
    and r1, r1, #1
    orr r1, r1, #(7 << 16)
    str r1, [r2, #0x78]
    ldr r0, =PRIV_VA

    @ watchdog -> timer mode, free running
    ldr r1, =0x12345678
    str r1, [r0, #WD_DISABLE]
    ldr r1, =0x87654321
    str r1, [r0, #WD_DISABLE]
    mvn r1, #0
    str r1, [r0, #WD_LOAD]
    ldr r1, =WD_CTRL_RUN
    str r1, [r0, #WD_CTRL]
    @ private timer stopped, status clear
    mov r1, #0
    str r1, [r0, #PT_CTRL]
    mov r1, #1
    str r1, [r0, #PT_ISR]
    @ GIC: priority 0 for IRQ 29, enable it, enable distributor + CPU interface
    add r2, r0, #GICD_CTRL           @ r2 = distributor base (offsets > 4 KiB)
    mov r1, #0
    strb r1, [r2, #(GICD_IPRIORITY - GICD_CTRL + IRQ_PTIMER)]
    mov r1, #(1 << IRQ_PTIMER)
    str r1, [r2, #(GICD_ISENABLER0 - GICD_CTRL)]
    mov r1, #0
    strb r1, [r2, #(GICD_IPRIORITY - GICD_CTRL + IRQ_PDC0)]
    mov r1, #1                                      @ SPI 42 -> CPU0
    add r3, r2, #0x800
    strb r1, [r3, #IRQ_PDC0]
    mov r1, #(1 << (IRQ_PDC0 - 32))
    str r1, [r2, #(GICD_ISENABLER0 - GICD_CTRL + 4)]
    ldr r1, [r2]
    orr r1, r1, #1
    str r1, [r2]
    mov r1, #0xF0
    str r1, [r0, #GICC_PMR]
    mov r1, #1
    str r1, [r0, #GICC_CTRL]

    mov r1, #1
    str r1, st_inited
    msr cpsr_c, r12
    bx lr

@ ---------------------------------------------------------------------------
@ u64 get_abstime(cpu_data *r3) = ticks + cpu_data->abstime_offset(+0x50/+0x54)
@ STACKLESS: the kernel's fast mach_absolute_time path (SWI handler, ~0x80066290)
@ calls this hook with sp pointing at nothing usable. Only r0-r2, r12.
shim_get_abstime:
    ldr r2, st_inited
    cmp r2, #0
    moveq r0, #0
    moveq r1, #0
    bxeq lr
    mrs r12, cpsr
    cpsid if
    ldr r2, =PRIV_VA
    ldr r0, [r2, #WD_COUNT]
    mvn r0, r0                      @ count up
    ldr r2, st_last_up
    ldr r1, st_ticks_hi
    cmp r0, r2
    addlo r1, r1, #1                @ wrapped since the last read
    strlo r1, st_ticks_hi
    str r0, st_last_up
    msr cpsr_c, r12
    ldr r2, [r3, #0x50]
    adds r0, r0, r2
    ldr r2, [r3, #0x54]
    adc r1, r1, r2
    bx lr

@ u32 get_decrementer(cpu_data *r3): ticks left until the deadline (stackless)
shim_get_decrementer:
    ldr r2, st_inited
    cmp r2, #0
    moveq r0, #0
    bxeq lr
    ldr r2, =PRIV_VA
    ldr r0, [r2, #PT_COUNT]
    bx lr

@ void set_decrementer(u32 ticks /* r0 */, cpu_data *r3) - normal kernel context
shim_set_decrementer:
    ldr r2, st_panic_req
    cmp r2, #0
    bne shim_user_panic
    push {r0, r3, r4, lr}
    bl shim_init
    pop {r0, r3, r4, lr}
    str r0, [r3, #0x60]             @ cpu_data->decrementer, like the original
    cmp r0, #0
    moveq r0, #1
    ldr r2, =PRIV_VA
    mov r1, #0
    str r1, [r2, #PT_CTRL]          @ stop
    mov r1, #1
    str r1, [r2, #PT_ISR]           @ clear a stale event
    str r0, [r2, #PT_LOAD]          @ load (also reloads the counter)
    ldr r1, =PT_CTRL_RUN
    str r1, [r2, #PT_CTRL]
    bx lr

shim_user_panic:
    mov r2, #0
    str r2, st_panic_req
    adr r0, panic_msg
    ldr r12, =0x80015c7d            @ _panic (Thumb)
    blx r12
    b .
panic_msg:
    .asciz "ios3ds: panic requested from the 3DS (SELECT+START) - thread dump follows"
    .balign 4

@ ---------------------------------------------------------------------------
@ IRQ entry (patched over the kernel's IRQ handler at 0x8006648c). The kernel
@ keeps no IRQ-mode stack, so only pc-relative scratch words are used.
shim_irq:
    str r0, irq_save0
    str r1, irq_save1
    str r2, irq_save2
    ldr r0, =PRIV_VA
    ldr r1, [r0, #GICC_IAR]
    str r1, st_last_irq
    mov r2, r1, lsl #22
    mov r2, r2, lsr #22             @ interrupt ID
    cmp r2, #IRQ_PTIMER
    bne 2f
    mov r2, #1
    str r2, [r0, #PT_ISR]           @ clear timer event
    str r1, [r0, #GICC_EOIR]
    ldr r2, st_timer_irqs
    add r2, r2, #1
    str r2, st_timer_irqs
    mrs r2, spsr
    tst r2, #0x40                   @ FIQs masked where we interrupted?
    bne 3f
    @ become an FIQ: lr_fiq = lr_irq, spsr_fiq = spsr_irq, then the vector's 'mov pc, r9'
    mov r0, lr
    mov r1, r2
    msr cpsr_c, #0xD1               @ FIQ mode, I+F masked
    mov lr, r0
    msr spsr_cxsf, r1
    ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    mov pc, r9

3:  @ FIQ masked: retry shortly (64 ticks ~ 61 us) instead of losing the tick
    ldr r2, st_deferred
    add r2, r2, #1
    str r2, st_deferred
    mov r2, #64
    str r2, [r0, #PT_LOAD]
    ldr r2, =PT_CTRL_RUN
    str r2, [r0, #PT_CTRL]
    b 9f

2:  cmp r2, #IRQ_PDC0
    bne 5f
    @ ---- top-screen VBlank: ack PDC + GIC, maybe inject the S5L CLCD frame IRQ
    ldr r2, =(PDC_VA + PDC0)
    ldr r0, [r2, #0x78]
    and r0, r0, #1
    orr r0, r0, #(1 << 17)
    str r0, [r2, #0x78]
    ldr r0, =PRIV_VA
    str r1, [r0, #GICC_EOIR]
    ldr r2, st_vblanks
    add r2, r2, #1
    str r2, st_vblanks
    ldr r0, cfg_clcd_pa
    cmp r0, #0
    beq 9f
    ldr r1, =(AXI_VA - AXI_PA)
    add r0, r0, r1                  @ r0 = fake CLCD (VA)
    ldr r1, [r0, #0x08]
    tst r1, #1                      @ frame IRQ armed by the driver?
    beq 9f
    bic r1, r1, #1                  @ one-shot: the driver re-arms it on the next swap
    str r1, [r0, #0x08]
    mov r1, #1
    str r1, [r0, #0x0C]             @ frame IRQ status
    ldr r2, cfg_vic0_pa
    ldr r1, =(AXI_VA - AXI_PA)
    add r2, r2, r1                  @ r2 = fake VIC0 (VA)
    ldr r1, [r2, #0x134]            @ VICVECTADDR13 (as programmed by AppleARMPL192VIC)
    str r1, [r2, #0xF00]            @ -> VICADDRESS: "IRQ 13 pending"
    ldr r1, st_injected
    add r1, r1, #1
    str r1, st_injected
    @ enter the kernel's own IRQ path (the 2 instructions our patch displaced)
    ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    sub lr, lr, #4
    mrs sp, apsr
    ldr pc, =KERNEL_IRQ_CONT

5:  @ anything else
    ldr r0, =1023
    cmp r2, r0
    beq 9f                          @ spurious: no EOI
    ldr r0, =PRIV_VA
    str r1, [r0, #GICC_EOIR]
    ldr r2, st_other_irqs
    add r2, r2, #1
    str r2, st_other_irqs
9:  ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    subs pc, lr, #4

    .ltorg

