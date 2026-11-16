# ios3ds

**Apple's stock iOS 4.2.1, running natively on the Old Nintendo 3DS's ARM11.**

ios3ds boots the unmodified iPod touch 2G firmware (iPod2,1, build 8C148: XNU kernel, launchd, SpringBoard) on the 3DS's ARM11 MPCore. It is a native port, not an emulator.
- Apple's own kernelcache runs directly on the CPU.
- A small set of binary patches and a "shim" bridges the hardware differences.
- The 3DS's ARM9 acts as a monitor and I/O processor.

> **Status (2026-11-16):** in the project's own QEMU model of the 3DS (`-M nintendo3ds`), iOS boots all the way to the **SpringBoard home screen**. The icon grid, status bar and dock are drawn by Apple's unmodified display stack and mirrored onto the 3DS top screen. Not yet tested on a real console.

| Milestone | Goal | State |
|---|---|---|
| M0 | Research, kernel strategy, IPSW prep tool | done |
| M1 | `.firm` payload: on-screen console, SD log, loads kernel + device tree, boot_args, jumps to XNU | done (emulator) |
| M2 | XNU prints through our console path | done (emulator) |
| M3 | Root filesystem from SD, `launchd`, daemons | done (emulator) |
| M4 | Framebuffer (done), buttons (done in emulator: START = Home, SELECT = lock, L/R = volume), touch (driver written, untested: the emulator has no touchscreen model yet) | mostly done |
| M5 | Graphics: SpringBoard needs no GPU (QuartzCore's software compositor). OpenGL ES 1.1 works through our CPU engine `tools/ios3ds-gl` ([docs/gpu.md](docs/gpu.md)); a PICA200 back end is future work | home screen reached without a GPU; `ios3ds-gltest` passes and draws in the emulator |

## How it works

```
3DS (Luma3DS chainloader)
 ├─ ARM9  "monitor" ── consoles on both LCDs, sd:/ios3ds/log.txt, loads everything from SD,
 │                     serves XNU's root disk (md0) from sd:/ios3ds/rootfs.hfs,
 │                     mirrors iOS's framebuffer onto the top LCD
 └─ ARM11 core 0 ───── stub: CPU probes, VFP on → jumps into Apple's XNU (core 1 stays parked)
      XNU 1504.58.28 (stock kernelcache + kernel/patches.json + kernel/shim)
        ├─ timer:      MPCore watchdog (time base) + private timer (deadlines), GIC IRQ 29 → kernel's FIQ handler
        ├─ console:    pexpert's S5L UART ops → shim → log ring → ARM9 → screen + SD
        ├─ root disk:  memdev (md0) strategy → shim → ARM9 block server → FAT file on SD
        ├─ display:    Apple's AppleM2CLCD on a fake register page; 3DS VBlank (GIC 42) → CLCD frame IRQ
        └─ IOKit platform kexts start against "fake" S5L8720 register pages in AXI WRAM
      launchd → configd, notifyd, syslogd, mediaserverd, … → SpringBoard (home screen)
```

See [docs/architecture.md](docs/architecture.md) for the full picture.

## Documentation
- [docs/architecture.md](docs/architecture.md): the boot chain end to end, memory map, every hardware bridge
- [docs/research.md](docs/research.md): hardware and firmware facts, with sources
- [docs/kernel-strategy.md](docs/kernel-strategy.md): why we patch Apple's kernel instead of using an open-source XNU port
- [docs/m1-design.md](docs/m1-design.md): the 3DS payload, mailbox protocol, `boot.cfg`
- [docs/m2-first-panic.md](docs/m2-first-panic.md): first kernel output and panic
- [docs/emulator.md](docs/emulator.md): the `nintendo3ds` QEMU development machine
- [docs/real-hardware.md](docs/real-hardware.md): testing on a real console safely: backups, stages, why nothing can touch NAND
- [docs/re-log.md](docs/re-log.md): reverse-engineering log, step by step, with addresses

## No Apple files in this repository

Nothing in this repository is Apple's. You supply your own IPSW, and `make` generates everything Apple-derived into `build/` (gitignored).

The patch recipes (`kernel/patches.json`, `rootfs/recipe.json`) contain only addresses and the few original bytes needed to check that the right firmware is being patched.

## Build

Requirements: Linux, Python 3 with `cryptography` (or the openssl CLI), an `arm-none-eabi` toolchain (devkitARM or the Arm GNU Toolchain), `mtools`, `dosfstools`, `sfdisk`, and git.

```sh
make toolchain                                          # check tools
make fwprep IPSW=~/iPod2,1_4.2.1_8C148_Restore.ipsw      # decrypt kernel, device tree, rootfs -> build/staging
scripts/build-hfsplus.sh                                # HFS+ image editor (xpwn) for the rootfs recipe
make sd                                                 # SD card tree -> build/sd/
make test                                               # offline unit tests
```

`fwprep` fetches the publicly documented firmware keys for that single build from api.ipsw.me, or takes `--keys file.json`.

Optional:

```sh
make qemu      # build QEMU with the nintendo3ds machine (~10 min)
make emu       # boot build/sd.img in it; prints sd:/ios3ds/log.txt, saves a screenshot
scripts/build-ios-toolchain.sh   # armv6 iOS Mach-O toolchain (ld64/cctools-port, ldid) for userland tools
```

## Try it on a console (SD only, nothing touches NAND or FIRM)
1. Run `make sd`, then copy the **contents** of `build/sd/` to the root of the SD card. That includes `luma/payloads/ios3ds.firm` and the `ios3ds/` folder (about 1 GB).
2. Hold **START** at power-on and choose `ios3ds` in Luma3DS's chainloader menu, **or** start `ios3ds` from the Homebrew Launcher. That reboots into ios3ds, and ios3ds puts Luma back as `boot.firm` first thing; see [docs/real-hardware.md](docs/real-hardware.md).
3. What you should see:
   - **Bottom screen:** the version, "SD ok", "ARM11 ok", then a countdown. Hold **B** to stay in the monitor.
   - **Top screen:** the boot log, including XNU's own messages. Once SpringBoard draws, the top screen shows iOS; turn the console 90° clockwise to read it.
4. Hold **SELECT+START** to make XNU panic and dump every thread's backtrace into the log.
5. Send back `sd:/ios3ds/log.txt` (the previous run is kept as `log_prev.txt`).

The first real-hardware runs are expected to fail early. Every stage writes a readable message to the screen and to `log.txt`.

## Limitations and notes
- **Why iOS 4.2.1 and not later:** later iOS versions need ARMv7; the 3DS ARM11 is ARMv6.
- **Not a 3DS homebrew app:** a `.3dsx` app runs in user mode under the 3DS's own OS, so it can't host another kernel. The `ios3ds.3dsx` launcher swaps `boot.firm` on the SD card and reboots instead. Likewise, Citra/Azahar emulate 3DS *applications* at a high level and can't run a bare-metal FIRM, which is why this project has its own QEMU board.
- **Missing hardware support:**
  - touch is untested (buttons work in the emulator);
  - no audio, Wi-Fi or real-time clock (the clock shows 1969/1970 time);
  - no data-protection keybag (keybagd is disabled; lock state is stubbed);
  - no activation (liblockdown is stubbed to report "Activated" for research);
  - OpenGL ES 1.1 is CPU-rendered (`tools/ios3ds-gl`): no PVRTC textures, no mipmaps or stencil yet, and no ES 2.0 (the MBX engine slot is ES1 only).

## Credits and prior art
- [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios) (Light Touch): research notes on booting 8C148, which were invaluable.
- [devos50/qemu-ios](https://github.com/devos50/qemu-ios): iPod touch emulation and S5L8720 hardware notes.
- [darwin-on-arm](https://github.com/darwin-on-arm/xnu) (winocm): boot_args and pexpert reference.
- [libn3ds / open_agb_firm](https://github.com/profi200/libn3ds), [Luma3DS](https://github.com/LumaTeam/Luma3DS), [linux-3ds](https://github.com/linux-3ds): 3DS hardware references.
- Normmatt's SD driver; FatFs by ChaN; [xpwn](https://github.com/planetbeing/xpwn) (img3/HFS+ tooling, used as an external build tool).
- The Apple Wiki and 3dbrew.

## License
GPL-3.0-or-later (`LICENSE`), with these exceptions:
- `boot/arm9/sdmmc.c` and `emu/qemu/` are GPL-2.0-or-later.
- `boot/arm9/fatfs/` is FatFs's 1-clause BSD licence.
- `boot/common/font8x8_basic.h` is public domain.

iOS, iPod and XNU are Apple's; Nintendo 3DS is Nintendo's. This is an independent research project.
