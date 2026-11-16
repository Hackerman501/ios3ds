# M2: XNU early boot as far as the first panic

**Status:** reached in the `nintendo3ds` emulator on 2026-11-16, with the stock
iOS 4.2.1 kernelcache plus 11 patches. Real-hardware run pending.

## Console path (works)

```
XNU printf/kprintf/panic ──► pexpert serial ops (S5L UART driver, 0x801dcdd0..)
     patched: tx-ready → "always ready", tx-data → ldr pc,=shim_putc
shim_putc (kernel/shim/shim.S, VA 0x80826000) ──► log ring @ PA 0x2082A000
     (cleans each touched D-cache line + DSB, so the uncached ARM9 sees it)
ARM9 monitor ──► top screen + sd:/ios3ds/log.txt
```

`serial=3` in the boot-args selects the serial console. The `debug` and AMFI
boot-args work because the generated device tree sets `/chosen/debug-enabled=1`.

## First panic log (verbatim from `sd:/ios3ds/log.txt`, emulator run)

```
jumping: ARM11 -> 20067050, r0=20825000 (boot_args)
ARM11 acknowledged, now in XNU
[monitor] XNU running - ARM9 stays here forwarding logs.
[a11] jumping to 20067050 r0=20825000
panic(cpu 0 caller 0x8006ab48): "sleh_abort at interrupt context"@/SourceCache/xnu/xnu-1504.58.28/osfmk/arm/trap.c:484
Debugger message: panic
OS version: Not set yet
Kernel version: Darwin Kernel Version 10.4.0: Wed Oct 20 20:06:26 PDT 2010; root:xnu-1504.58.28~3/RELEASE_ARM_S5L8720X
iBoot version: ios3ds-loader
secure boot?: NO
Paniclog version: 1
Debugger message: panic
...
*********  Dumping thread state and stacks *********
panic: We are hanging here...
[monitor] XNU entry stub ran: L1[0x800] = 2004040e (TTBR region 20900000)
```

## Analysis

QEMU `-d int` shows the abort behind the panic:

```
Taking exception 4 [Data Abort] ... DFSR 0x808 DFAR 0xc0013088
```

- DFSR 0x808 is a precise **external** abort on a **write**. The address is
  mapped, but nothing on the bus answers.
- The faulting store is at 0x801dbf94 in `pe_identify_machine`'s `s5l8720x-io`
  branch.
  - It looks up `/arm-io/interrupt-controller=master`, `/arm-io/timer`
    (device_type `timer`) and `cpu-debug-interface`.
  - It `ml_io_map`s the timer at PA 0x3C700000 (arm-io 0x4700000 → 0x38000000 range).
  - It then writes the S5L "timer 4" config (+0x88 = `0x10018000 | (div-1)<<4`)
    and data (+0xA8 = 0x7FFFFFFF) registers.
- The 3DS has no peripheral at 0x3C700000. "at interrupt context" is reported
  because this runs in the very early platform init, before the kernel's
  interrupt bookkeeping is set up.

**How the 4.2.1 kernel uses that timer** (osfmk/arm, ARM mode, through a
state struct whose +0x74 is the register base):
- 0x80068038 `read 64-bit tick counter`: hi +0x80 / lo +0x84 with a retry
  loop, plus an offset at +0x50/+0x54 → `mach_absolute_time`
- 0x80068064 `time left`: +0xA8 − +0xB4
- 0x80068078 `set deadline`: +0xA8 = count, +0xA4 = 3 (START|MANUALUPDATE).
  samhenrigold/qemu-ios documents that this must auto-reload.
- the interrupt arrives as an **FIQ** (handler around 0x80068024). That
  handler uses FIQ-banked registers r8, r10 and r11, which the kernel sets up
  at init.

## Problems fixed to get this far (each is in `kernel/patches.json` or the generators)

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | `pe_identify_machine: Epoch Mismatch` | 4.2.1 requires boot_args `Version == 5` (public reimplementations use 1/2) | `ba_version=5` in boot.cfg |
| 2 | spin in `uart tx-ready` | S5L UART doesn't exist | 6 UART-op patches → shim ring |
| 3 | data abort (translation) at 0xC0001000 | `/pram` reg left 0 (iBoot fills it); the panic-log 'OOPS' check reads it | `/pram` = AXI WRAM 0x1FFA0000+64K, `/vram` = VRAM 0x18000000+3M |
| 4 | (preventive) ARM1176-only CP15 | PPMR ×2, IFAR ×2, ACTLR | 5 patches (research.md §3.4) |

## Next step: timer and interrupts (the M3 entry ticket)

1. **Fake S5L timer page.** Point `/arm-io/timer` `reg` at a 4 KiB page in
   AXI WRAM. In arm-io child space, 0x1FFB0000 is 0x17FB0000 via the
   0x10000000→0x18000000 range. The early config writes then land in RAM, and
   the kernel's "register base" becomes plain memory the shim can watch.
2. **Real time base.**
   - Patch the three primitives (0x80068038 / 0x80068064 / 0x80068078) to
     branch into shim functions.
   - The 64-bit counter comes from the MPCore **watchdog in timer mode**
     (free-running 32-bit down-counter, extended to 64 bits in the shim).
   - Deadlines come from the **private timer** in one-shot mode (GIC IRQ 29).
   - `timebase-frequency` in the device tree must match the prescaled clock
     (e.g. 134,055,928 / 256 ≈ 523.7 kHz).
3. **Interrupt entry.** The MPCore GIC delivers IRQ, not FIQ.
   - Patch the kernel's IRQ vector into a shim trampoline that reads
     GICC_IAR (0x17E0010C).
   - For IRQ 29: clear the timer status, write EOI, then re-enter the
     kernel's **FIQ** handler as if a real FIQ had been taken. The shim copies
     lr/spsr into FIQ mode through pc-relative scratch words and never touches
     the kernel's FIQ-banked r8–r12.
   - Every other IRQ goes to the kernel's normal IRQ path, which later reaches
     our GIC IOInterruptController.
4. **Stop S5L8720 kexts from matching.** Remove `vic`, `edgeic`, `gpio`,
   `power`, `clkrstgen`, `dmac*`, `usb-complex`, `clcd`, `flash-controller0`,
   `sdio`, `mbx` and friends from the device tree as each reaches the front.
   That follows the plan in docs/research.md §4.
5. **Then:** the next panics will come from IOKit (platform expert,
   interrupt controller) and from the root device. A RAM-disk root via
   memory-map `RAMDisk` + `rd=md0` with a tiny static `launchd` test binary is
   the cheapest path to "launchd attempt". The real rootfs needs an SD block
   driver; the ARM11 can own the SD slot via CFG9_SDMMCCTL (research.md §1.4).

## Reproduce

```sh
make fwprep IPSW=.../iPod2,1_4.2.1_8C148_Restore.ipsw
make qemu        # once
make emu         # → build/emu/log.txt, build/emu/screen_009.5s.png
python3 emu/run.py --time 9 --trace int   # → build/emu/qemu.log with the abort above
```
