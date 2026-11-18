# ios3ds architecture

How stock iOS 4.2.1 (iPod2,1, 8C148) reaches the SpringBoard home screen on an Old 3DS. All addresses are for this one firmware build.

## 1. Boot chain

1. **Luma3DS** chainloads `luma/payloads/ios3ds.firm`. Luma's screen-init flag is set, so the LCDs come up with BGR8 framebuffers.
2. **ARM9 monitor** (`boot/arm9`, runs from ARM9 RAM):
   1. Draws consoles on both LCDs and opens `sd:/ios3ds/log.txt`.
   2. Handshakes with the ARM11 through a mailbox in AXI WRAM.
   3. Reads `boot.cfg` and loads `kernel.bin`, `devtree.bin`, `shim.bin` and `fakeio.bin`.
   4. Builds XNU's `boot_args` (Version must be **5**).
   5. Opens `rootfs.hfs` for the block server and tells the ARM11 to jump.
   6. Stays alive forever as the monitor.
3. **ARM11 stub** (`boot/arm11`): reads the CP15 ID registers, optionally probes CPU instructions, enables VFP, then jumps to the kernel entry point (PA 0x20067050) with r0 = boot_args, MMU off. Core 1 stays parked in boot11.
4. **XNU** builds its page tables at `topOfKernelData`, turns on the MMU (virtual 0x80000000 = physical 0x20000000), and boots.

## 2. Physical memory

| Range | Use |
|---|---|
| 0x20001000 | kernelcache, flat image with patches applied (8.1 MB) |
| after the kernel | device tree, then boot_args |
| 0x20840000 | **kernel shim** (`kernel/shim`, linked at virtual 0x80840000): code below +0x5F00, +0x5F00 probe snapshot, +0x5F80 IRQ PC samples, +0x6000 block mailbox, +0x6200 input mailbox, +0x6400 MCU mailbox ([mcu.md](mcu.md)), +0x7F00 GPU mirror mailbox, +0x8000 32 KB bounce buffer |
| 0x20850000 | XNU log ring (256 KB) |
| 0x208A0000 | network mailbox (docs/wifi.md) |
| 0x208B0000 | `gpu.bin`: the shim's GPU part (32 KB, linked at 0x808B0000) + the display mirror's PICA200 command lists at 0x208B8000 ([pica200.md](pica200.md) §9) |
| 0x208C0000 | audio PCM ring (docs/audio.md) |
| 0x20900000 | topOfKernelData (kernel page tables); XNU owns everything up to 0x28000000 |
| 0x1FF90000 | ARM11 stub (AXI WRAM) |
| 0x1FFA0000 | `/pram` (panic log area) |
| 0x1FFB0000 | **fake S5L8720 register pages** (32 × 4 KB, `fakeio.bin`) |
| 0x1FFF0000 | ARM9 ↔ ARM11 mailbox |
| 0x18000000 | `/vram`: iOS's framebuffer (window 1, 320×480, 32 bpp) |
| 0x18300000 | Luma's LCD framebuffers (the ARM9 draws here): top/bottom 0x18300000/0x18346500 and 0x18400000/0x18446500 |
| 0x18480000 | GPU display mirror: upload buffer (320x480) and the two render targets, to 0x185BEC00 (its 512x512 texture is in the FCRAM arena); GPU diagnostics scratch 0x185C0000-0x185D4000 |

## 3. Bridging S5L8720 to the 3DS

**CPU** (ARM1176 → ARM11 MPCore), patched in `kernel/patches.json`:
- `c15,c2,4` (peripheral port remap) becomes a NOP.
- IFAR, which MPCore lacks, is replaced by the abort handler's own `lr` (the handler already subtracts 4).
- ACTLR writes are ignored, because bit 5 means SMP on MPCore.

**Device tree** (`kernel/dtgen.py`), generated from Apple's n72ap template:
- Fills the values iBoot would fill: memory, memory-map, CPU frequencies, `debug-enabled`, `/pram`, `/vram`.
- Deletes the peripherals the 3DS doesn't have.
- Removes `secure-root-prefix`.
- Points the platform devices iOS insists on at fake register pages: timer, VIC, edge IC, GPIO, power, clock/reset, watchdog, UART0 and CLCD.
- `arm-io` `ranges[0]` maps child address 0 to the fake pages, because pexpert only uses the first range. `ranges[1]` is an identity map of the real 3DS IO window.

**Console:**
- pexpert's built-in S5L UART ops are patched; "transmit" becomes `shim_putc`.
- `shim_putc` writes the log ring and cleans the D-cache lines.
- The ARM9 drains the ring to the screen and to SD.

**Time and interrupts:**
- XNU keeps per-CPU timer hooks: `cpu_data+0x64/0x68/0x70` (decrementer get/set, absolute time) and `+0x6C` (FIQ handler, held in r9_fiq). The three S5L implementations jump into the shim.
- Absolute time comes from the MPCore watchdog in timer mode (free-running, extended to 64 bits).
- Deadlines use the MPCore private timer.
- Both run at 1,047,311 Hz, which is the device tree's `timebase-frequency`.
- The IRQ vector enters `shim_irq`:
  - **GIC 29 (timer):** acknowledge, then re-enter the kernel's FIQ handler as if a real FIQ had been taken.
  - **GIC 42 (3DS top LCD VBlank):** set the fake CLCD frame-status bit, put the CLCD vector into the fake VIC's VICADDRESS, and enter the kernel's normal IRQ path. Apple's VIC and CLCD drivers then handle it like real hardware.
- The time hooks are **stackless**: the `mach_absolute_time` fast path calls them without a usable stack.

**System controller (MCU)** ([mcu.md](mcu.md)):
- The ARM9 polls the MCU over I2C and fills the MCU mailbox (shim + 0x6400).
- `PEGetGMTTimeOfDay` / `PESetGMTTimeOfDay` / `PEHaltRestart` go to the shim: the calendar starts from the 3DS RTC, and a shutdown powers the console off through the MCU.
- The shim registers an `IOPMPowerSource` for SpringBoard's battery icon; HOME and POWER reach iOS as input-mailbox buttons; `ios3ds-mcud` handles volume, brightness and the accelerometer.

**Root disk:**
- The device tree's memory-map gets a `RAMDisk` entry the size of the root filesystem, and the boot-args include `rd=md0`.
- `mdevstrategy` (bsd/dev/memdev.c) jumps into `shim/blk.c`.
- Each transfer goes through the block mailbox and bounce buffer, with interrupts masked and the buffer pages pre-faulted.
- The ARM9 serves it from `sd:/ios3ds/rootfs.hfs` with FatFs fast seek.

**Display:**
- Apple's AppleM2CLCD driver runs against a fake register page that starts with iBoot's state (window 1 at 0x18000000).
- QuartzCore finds the `AppleM2CLCD` service. It composites with OpenGL ES through our engine on the PICA200 when the GPU passes its checks ([gpu.md](gpu.md) §7), else with its **software compositor**; either way rendering is paced by vsync notifications, i.e. the injected VBlank.
- Window 1 goes to the 3DS LCDs (top: rotated and scaled to 3/4, 360×240; bottom: 2/3, the touch map). The ARM11 shim does it on the PICA200 once its self-test passed (the GPU display mirror, [pica200.md](pica200.md) §9); until then, or when it is switched off or fails, the ARM9 copies with the CPU.

## 4. Userland changes

These are applied to a copy of your root filesystem by `scripts/mkrootfs.py` from `rootfs/recipe.json`:
- `/etc/fstab`: `/dev/md0 / hfs rw`. The volume is grown to 1 GiB, because Apple's image has no free blocks.
- About 28 launch daemons for absent hardware are disabled: baseband, Bluetooth, Wi-Fi, accessories, keybagd, datamigrator, lockdownd, …
- The dyld shared cache gets small byte patches:
  - liblockdown is stubbed, reporting "Activated" and BrickState = false;
  - `AppleKeyStoreGetDeviceLockState` returns "no passcode" (without a system keybag the kernel call blocks forever).
- SpringBoard's stdout and stderr go to `/dev/console`. Its environment has `MBX2D_PAGE_FLIP=0`, `CA_ENABLE_OGL=1` and `IOS3DS_CA=1` (CoreAnimation composites with OpenGL ES on the PICA200 when the GPU passes its checks, else in software: [gpu.md](gpu.md) §7), and `com.apple.springboard.plist` sets `SBDontLockAfterCrash` and `SBDisableAutoDim` (`SBDontLockEver` was removed: lock now turns the screen off, see [mcu.md](mcu.md) §6.3).
- OpenGL ES 1.1 comes from our engine `tools/ios3ds-gl`, installed as `OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine`:
  - dyld's `enable-dylibs-to-override-cache` lets it replace the cached copy;
  - OpenGLES's `"AppleMBXDevice"` lookup is patched to `"IOResources"`;
  - QuartzCore's ES1 display surface format is patched from 16 to 32 bpp, so its GL compositor can run on our engine (SpringBoard only, GPU only, [gpu.md](gpu.md) §7);
  - see [gpu.md](gpu.md). `ios3ds-gltest` checks it at boot.
  - with `IOS3DS_GL=pica` the engine renders on the PICA200 through the shim's `ios3ds_gpu` service (`kernel/shim/pica.c`); see [pica200.md](pica200.md).
- `ios3ds-sample` (`tools/ios3ds-sample`) prints every process's thread state to the console periodically. It is a debugging aid.

## 5. Debugging aids
- **SELECT+START:** XNU panics and dumps all thread backtraces, through the shim's `panic_req`.
- `emu/run.py`, `emu/why.py`, `emu/mon.py`: run headless, explain the first exception, poke a stopped VM.
- `scripts/kexts.py`, `scripts/btmap.py`, `scripts/ksym.py`: kext load addresses and personalities, and backtrace annotation.
