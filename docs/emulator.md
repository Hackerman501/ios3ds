# The `nintendo3ds` QEMU machine (development aid)

As of 2026-11 there is no 3DS machine in upstream QEMU, and none online
(2020 qemu-discuss thread; existing 3DS emulators are HLE or need dumped
boot ROMs). So ios3ds ships its own minimal model:
`emu/qemu/hw/arm/nintendo3ds.c`, built into QEMU v10.2.4 by `make qemu`.

**It is not a 3DS emulator.** It models only what the ios3ds payload and the
early XNU boot touch. Everything we learn from it still has to be confirmed
on the console.

## What is modelled

| Part | Model |
|---|---|
| ARM11 MPCore core 0 | QEMU `arm11mpcore` (core 1 is not created, which matches "parked") |
| ARM9 | QEMU `arm946`, with its own address space (ITCM + mirrors, ARM9 RAM, DTCM, ARM9-only IO) |
| CPU clusters | separate clusters, because QEMU requires that for different CPU types |
| MPCore private region | `arm11mpcore_priv` at 0x17E00000 (SCU, GIC with 128 IRQs, private timer, watchdog) |
| RAM | FCRAM 128 MiB, VRAM 6 MiB, AXI WRAM 512 KiB, DSP RAM, all shared between both CPUs |
| Boot ROMs | stub vector ROMs: boot9 → 0x08000000 veneers, boot11 → 0x1FFFFFA0 RAM vectors, plus the boot11 wait loop on 0x1FFFFFFC |
| Luma chainload | `-kernel x.firm`: sections copied, ARM9 gets argc=2 and argv with the fb pointers when the FIRM sets screen-init, ARM11 jumps to its entry. MMU/caches off. |
| SD | TMIO host at 0x10006000 (the command, status, FIFO32 and auto-CMD12 subset that Normmatt's driver uses) on top of QEMU's real SD card model (`-drive if=sd`) |
| Screens | PDC0/PDC1 framebuffer registers; formats 0–4, rotated storage; a 400×480 display (top screen above bottom). `screendump` gives PNGs. |
| Buttons | HID at 0x10146000. Keyboard: A=a, B=s, X=x, Y=y, L=q, R=w, START=Enter, SELECT=Backspace, plus the arrow keys |
| CFG9 / CFG11 | SDMMCCTL is stored; SOCINFO reports an Old 3DS |
| Unmapped addresses | **external abort**, like a real bus. This is how the emulator caught XNU touching S5L8720 peripherals. Known IO windows are `unimplemented-device` (reads return 0, logged with `-d unimp`). |

## Known fidelity gaps (verify on hardware)

- **CP15 encodings missing from MPCore don't trap in QEMU**:
  `c15,c2,4` and `c6,c0,2` read back 0. The real behaviour is
  *UNPREDICTABLE* per the TRM, so the M1 probes report what the console
  actually does. The kernel patches remove both encodings regardless.
- No cycle accuracy. Busy-wait delays run faster than on hardware: the
  "monitor alive" counter climbs about 10× faster.
- Caches are not modelled, so coherency bugs won't show up here. That's why
  the design keeps caches off everywhere except inside XNU, and the shim
  cleans every line it writes.
- No PXI, I2C, MCU, GPU, DSP or SPI. The LCDs are already "on", as after
  Luma's screen init.

## Usage

```sh
make qemu                    # once (~10 min)
make sdimg                   # payload + kernel + devtree + shim -> build/sd.img
make emu                     # boot, screenshot at 9.5 s, print sd:/ios3ds/log.txt
python3 emu/run.py --time 10 --trace int,unimp,guest_errors   # exceptions -> build/emu/qemu.log
python3 emu/run.py --regs                      # register dump of both CPUs at the end
python3 emu/mon.py --boot 7 "info registers" "x/16wx 0x80251ef0"   # poke a stopped VM
python3 emu/run.py --gdb     # waits for arm-none-eabi-gdb on :1234
```
