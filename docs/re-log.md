# Reverse-engineering log

Every analysis step, with the command and what it showed, so anyone can
re-run it against their own IPSW. Newest entries go at the end.

Inputs: `iPod2,1_4.2.1_8C148_Restore.ipsw` (363,553,480 bytes, sha1
`b9efddc7bb4350c237a8d3846af61bbfc8a2f647`, same as the ipsw.me listing for
Apple's `secure-appldnld.apple.com/iPhone4/061-9855.20101122.Lrft6/...` URL).
Keys: `https://api.ipsw.me/v4/keys/ipsw/iPod2,1/8C148`. The Apple Wiki puts a
CAPTCHA in front of automated clients, so the tooling doesn't scrape it.

## 2026-11-16: M0

### 1. IPSW contents
`python3 -c "import zipfile; ..."` lists:
- `kernelcache.release.n72` (IMG3, 4.37 MB)
- `Firmware/all_flash/all_flash.n72ap.production/DeviceTree.n72ap.img3`
- root filesystem `038-0014-002.dmg` (311 MiB)
- restore ramdisk `038-0049-002.dmg` and update ramdisk `038-0050-002.dmg`
- iBoot, LLB, iBEC and iBSS, plus the boot logos

### 2. IMG3 decryption gotcha
The kernelcache IMG3 has the tags TYPE, DATA, SEPO, KBAG, SHSH and CERT.
Decrypting only `dataLength & ~15` bytes of DATA (as some docs describe)
leaves a 15-byte encrypted tail, and LZSS then comes up 64 bytes short with an
adler32 mismatch. The ciphertext is padded to 16 bytes inside the tag's slack
space. Decrypting `roundup(dataLength, 16)` bytes and truncating fixes it, and
the adler32 then matches (`1c4f9e86`). Fixed in `scripts/fwlib/img3.py`.

### 3. Kernel identity and layout (`make kscan`)
- `Darwin Kernel Version 10.4.0: Wed Oct 20 20:06:26 PDT 2010; root:xnu-1504.58.28~3/RELEASE_ARM_S5L8720X`
- Mach-O: cputype 12 (ARM), cpusubtype 6 (`CPU_SUBTYPE_ARM_V6`), MH_EXECUTE.
- `__TEXT` vmaddr **0x80001000**; entry (LC_UNIXTHREAD pc) **0x80067050**.
  The kernel lives at 0x80000000 and up, not 0xC0000000.
- Segments: `__TEXT`, `__DATA`, `__KLD`, `__LAST`, `__PRELINK_TEXT`
  (0x802cf000, 5 MB of kexts), `__PRELINK_INFO` (XML), `__CTF`, `__LINKEDIT`.
- LC_SYMTAB holds 4391 symbols. These are the exported KPI that kexts link
  against, e.g. `_ml_io_map`, `_PE_cpu_start`, `_ml_processor_register`,
  `_IOFlushProcessorCache`, `_kprintf`, `_panic`, `_PE_parse_boot_argn`,
  `_PE_i_can_has_debugger`. Internal symbols such as `_arm_init` and
  `_PE_init_platform` are stripped.
- 99 distinct prelinked kext bundle IDs (see `build/staging/kernelcache.txt`).
- Panic strings include source paths. The ARM-specific files are
  `osfmk/arm/{cpu,locks_arm,loose_ends,machine_routines,pcb,pmap,rtclock,status,trap}.c`,
  `osfmk/console/arm/serial_console.c`, `osfmk/console/video_console.c`,
  `bsd/dev/arm/{systemcalls,unix_signal}.c` and `osfmk/kdp/ml/arm/kdp_machdep.c`.

### 4. Entry stub disassembly (0x80067050)
I wrapped `__TEXT` in an ELF and disassembled it:
`llvm-objcopy -I binary -O elf32-littlearm` then
`llvm-objdump -D --triple=armv6k-none-eabi --adjust-vma=0x80001000`.
The stub runs in ARM mode with the MMU off:
```
mov r1,#0 ; ldr lr,=<virt continuation> ; cpsid if
ldr r8,[r0,#8]    ; boot_args->physBase
ldr r9,[r0,#4]    ; boot_args->virtBase
ldr r10,[r0,#0xc] ; boot_args->memSize
ldr r4,[r0,#0x10] ; boot_args->topOfKernelData  -> L1 table goes HERE
TTBR0 = TTBR1 = r4 | 0x18 ; TTBCR = 2
zero 9*1024 words at r4 (16 KB L1 + 20 KB of L2 tables)
identity-map the 1 MB section containing PC (V=P, so the MMU-on switch survives)
map [virtBase, virtBase+memSize) -> physBase as SUPERSECTIONS (bit18), C+B, AP=01
L2 page table at r4+0x8000 for 0xFFF00000; maps the vector page at 0xFFFF0000
clean+invalidate D-cache by set/way: 128 sets x 4 ways x 32 B  (16 KB geometry hard-coded)
SCTLR |= M|C|W|Z|I|V|U|XP (0x00C0380D), high vectors, ARMv6 extended page tables
r0 = r0 - physBase + virtBase ; jump to virtual continuation
```
What this means for the loader:
- r0 = **physical** pointer to boot_args, IRQ/FIQ masked, MMU off.
- `topOfKernelData` must be **16 KB aligned** (it becomes TTBR) and is followed
  by 36 KB that the kernel zeroes.
- physBase must be **16 MB aligned**, because supersections are used. FCRAM at
  0x20000000 qualifies.
- boot_args and the device tree must sit inside `[physBase, physBase+memSize)`
  so their virtual addresses are valid after the switch.
- Vectors are **high (0xFFFF0000, SCTLR.V)**, mapped by the kernel's own page
  table. VBAR is never used.

### 5. CP15 / secure-instruction scan (`scripts/kscan.py`)
This is a word scan of ARM-mode code. ARMv6 Thumb cannot issue coprocessor
instructions, so the scan is complete apart from literal-pool false positives.

Results:
- **No SMC, no MCRR block cache ops, no c12 (VBAR) access** in the kernel or the kexts.
- `MRC/MCR p15,0,c15,c2,4` at 0x80060274 (read) and 0x80064208 (write, in
  the reset/resume path of the vector page). This is the ARM1176 *Peripheral
  Port Memory Remap* register, so it needs patching.
- `MCR p15,0,c1,c0,1` (ACTLR write) at 0x80067bf0 is a setter function. It
  needs review because the MPCore ACTLR has the SMP/AMP bit.
- `MRC p15,0,c6,c0,2` (IFAR) at 0x800662e0 and 0x8006632c, in the
  prefetch-abort path.
- PMU `c15,c12,0..3` in the kernel (0x8006f1xx–0x8006f28c) and in a kext
  (AppleARM11xxProfile, 0x805810d4+).
- Everything else is standard ARMv6: c7 cache and barrier ops by MVA and
  set/way, `c7,c0,4` WFI, `c8` TLB ops, `c13,c0,1..4` (CONTEXTIDR and the thread
  ID registers; `c13,c0,4` = TPIDRPRW is read 106 times, it holds the
  current-thread pointer), TTBR0/1, TTBCR, DACR, DFSR, IFSR, FAR, CPACR.

### 6. SoC layer inside the kernel (strings plus pointer xrefs)
`pe_identify_machine` matches `/arm-io` `device_type` against a table at
0x801dc0e4–0x801dc4ac. The table has entries for: `s5l8900x-io`,
`s5l8720x-io`, `s5l8722x-io`, `s3c2410-io`, **`integratorcp-io`**,
`olocreek-io`, `omap3430sdp-io` and `s5i3000-io`. Each entry carries
clock/timebase constants (for example 200/100/50 MHz for Integrator/CP, and
19.2 MHz next to OMAP3430). The panic string is `pe_identify_machine: Epoch Mismatch`.

Built-in serial drivers are selected by `uart,16550`, `uart,s5i3000` and
`uart,s5l89x0x` (8900/8720/8920/8930/8940/8950). `boot-console`,
`-progress` and `BootCLUT` are also present, along with the 32 bpp CLUT
format string `BBBBBBBBGGGGGGGGRRRRRRRR` used by the video console.
**Implication:** early timer, serial and framebuffer console support live in the
kernel proper (pexpert, mostly Thumb code). Everything else is a kext matched
through the device tree.

### 7. Device tree template (`build/staging/devicetree.txt`)
77 nodes. Root `compatible = N72AP|iPod2,1|AppleARM`.
`/cpus/cpu0 compatible = ARM,1176jzf-s|ARM,v6`.
`/arm-io device_type = s5l8720x-io` with `ranges` = (0x00000000 → 0x38000000,
size 0x08000000) and (0x10000000 → 0x18000000, size 0x10000000). Children
include: vic (`vic,pl192`), edgeic, gpio, power, clkrstgen, dmac0/1, sha1,
usb-complex, amc, clcd, flash-controller0, aes, sdio, h264decoder, scaler,
tv-out, mbx, mpvd, prng, spi0/1/4 (multi-touch on spi4), otgphyctrl,
i2c0 (accelerometer, pmu, audio0, spkr-amp), i2c1 (als, mikey), timer (`timer,s5l8720x|timer,s5l8900x`),
wdt, i2s0, uart0/1 (bluetooth on uart1), pke, mipi-dsim/lcd, swi and xmc.
The template's `/chosen/memory-map`, `/memory reg` and the frequencies are all
zero, because iBoot fills them in. Wi-Fi is **Broadcom** (`AppleBCMWLAN`).

### 8. Root filesystem
The VFDecrypt key works. The decrypted UDIF has 7 blkx partitions; the
`disk image (Apple_HFSX : 5)` one is 1,287,512 sectors (629 MiB) and carries the
HFSX signature `HX`. `7z l` lists `sbin/launchd`, `usr/lib/dyld`,
`System/Library/CoreServices/SpringBoard.app/SpringBoard`, and
`System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6` (167 MB, larger than RAM!).
GL lives in `OpenGLES.framework/MBXGLEngine.bundle` (the only GL engine) and
`PrivateFrameworks/MBX2D.framework`. IOMobileFramebuffer, IOSurface and
CoreSurface are present.

## 2026-11-16: M1/M2 (emulator)

### 9. QEMU development machine
- Upstream QEMU has no 3DS machine, and none was found online.
- I wrote `emu/qemu/hw/arm/nintendo3ds.c` (docs/emulator.md), built against
  QEMU v10.2.4.
- The two different CPU types need separate `cpu-cluster` objects.
- Unmapped physical addresses raise external aborts, which is how the S5L8720
  accesses below were found.

### 10. M1 payload in the emulator
`make emu` gives:
- SD mounts (QEMU SD card, SDHC, 2 GiB);
- the ARM11 handshake works;
- CP15 ID registers are reported (QEMU arm11mpcore: MIDR 410fb022 r0p2,
  CTR 1d192992, FPSID 410120b4, MVFR0 11111111);
- the kernel loads and the jump happens.

Probing `c15,c2,4` and `c6,c0,2` does **not** trap in QEMU (they read 0). The
real console must answer that.

### 11. "pe_identify_machine: Epoch Mismatch"
- After the jump, the ARM11 PC was at 0x80015dd0, just past `_panic`.
- Return addresses on the stack (`x/48wx 0x80251ef0`) were 0x801dc137
  (pe_identify_machine), 0x801dc743 and 0x80248960.
- Disassembly at 0x801dc124: `ldrh r3,[r0,#2]; cmp r3,#5; beq ok; ldr r0,="pe_identify_machine: Epoch Mismatch"; bl _panic`.
- **boot_args.Version must be 5 for iOS 4.2.1.** It's now `ba_version=5` in boot.cfg.

### 12. Serial console = S5L UART ops table
- The kernel was spinning at 0x801dcef4 (`ldr r0,[base,#0x10]; and r0,#4`):
  the UTRSTAT TX-empty poll.
- The ops table at 0x80266e48: init 0x801dcdd0, baud 0x801dce5c,
  tx-ready 0x801dcee8, tx-data 0x801dcf00 (`strb r0,[base,#0x20]` = UTXH),
  rx-ready 0x801dcf18, rx-data 0x801dcf30.
- A DCC (cp14) variant exists at 0x80266e30.
- All six were patched. tx-data jumps into `kernel/shim/shim.S`, which writes
  to the ring that the ARM9 drains.
- Result: the kernel's own panic text appears on the 3DS screen and in log.txt.

### 13. `/pram` must be filled
- Translation-fault data abort at VA 0xC0001000, at PC ~0x801dcb8a.
- The kernel reads the DT `reg` of the `/pram` node, `ml_io_map`s it, and
  compares the first word with 0x4F4F5053 ('OOPS', the persistent panic log).
- The template's reg is (0,0), so nothing was mapped.
- Fixed in kernel/dtgen.py: `/pram` = AXI WRAM 0x1FFA0000 (64 KiB),
  `/vram` = VRAM 0x18000000 (3 MiB). Both lie outside XNU's RAM, as iBoot's
  carve-outs do.

### 14. First real panic: the S5L timer
`sleh_abort at interrupt context` (trap.c:484):
- external abort, write to VA 0xC0013088;
- the store at 0x801dbf94 writes the timer-4 config register in the
  s5l8720x branch of `pe_identify_machine` (its literals: "interrupt-controller",
  "master", "timer", "cpu-debug-interface", `_gPEClockFrequencyInfo`).

The timer primitives in osfmk/arm are:
- 0x80068038: 64-bit counter at +0x80/+0x84
- 0x80068064: time left
- 0x80068078: set deadline (+0xA8 count, +0xA4 = 3)

The FIQ handler is near 0x80068024 and uses FIQ-banked r8, r10 and r11.
Analysis and plan: docs/m2-first-panic.md.

## 2026-11-16: from the first panic to SpringBoard drawing (emulator)

### 15. Timer and interrupts
- XNU-1504's timer is pluggable through per-CPU hooks:
  - `cpu_data+0x6C`: the FIQ handler, loaded into r9_fiq
  - `+0x74`: the timer register base
  - `+0x70` / `+0x64` / `+0x68`: get-abstime / get-decrementer / set-decrementer, implemented at 0x80068038 / 0x80068064 / 0x80068078
- The three hooks are patched into the shim:
  - time base = MPCore watchdog in timer mode (free-running, extended to 64 bits)
  - deadlines = MPCore private timer (GIC 29)
  - both at PERIPHCLK/128 = 1,047,311 Hz, which is also `timebase-frequency`
- The IRQ entry (0x8006648C) is patched into a trampoline. For IRQ 29 it re-enters the kernel's **FIQ** handler (`mov pc, r9` in FIQ mode).
- **Gotcha:** the SWI fast path for `mach_absolute_time` (~0x80066290) calls get-abstime with no usable stack (sp just under the vector page), so that hook is stackless.
- `/arm-io/timer` and the VIC, edge IC, GPIO, power, clkrstgen, watchdog and uart0 nodes point at fake register pages in AXI WRAM (0x1FFB0000+).
- pexpert translates child addresses as `ranges[0].parent + child` and ignores the other ranges, while IOKit checks every range. So `ranges[0]` = child 0 → fake pages, and `ranges[1]` = identity map of the 3DS IO window.
- AppleARMPL192VIC needs its PrimeCell ID registers (0xFE0–0xFFC) filled in (`fakeio.bin`).

### 16. IOKit and the root device
- RTC wait: 30 s timeout patched to 0 (0x802E78B4).
- `IOPMU` waits only block side threads.
- `secure-root-prefix=md` made the md root wait for a SecureRoot callback, so the property is removed.
- Root = md0 via memory-map `RAMDisk` + `rd=md0`. `mdevstrategy` (0x80072BF8, Thumb) is patched into `kernel/shim/blk.c`, which hands I/O to the ARM9 block server reading `sd:/ios3ds/rootfs.hfs` with FatFs fast seek.
- Bugs found on the way:
  - preemption race on the single mailbox → interrupts off per transfer;
  - lazily mapped UPL pages → pre-fault them before masking interrupts.

### 17. Prefetch-abort fault address
The IFAR replacement was `lr-4`, but the handler had already done `sub lr,lr,#4` (0x800662AC). The result was 2 million prefetch aborts at the same address; the fix is `mov r1/r5, lr`.

### 18. Userland
- launchd starts.
- Root filesystem changes (`rootfs/recipe.json`, applied with xpwn hfsplus):
  - fstab `/dev/md0 / hfs rw`
  - grown to 1 GiB (after removing CoreLocation's consolidated.db, since Apple's image has 0 free blocks)
  - 28 daemons disabled (keybagd: FATAL KEYBAG ERROR → restore reboot)
  - SpringBoard stderr routed to `/dev/console`
- SpringBoard fixes:
  - "No window server display found" → keep `arm-io/clcd` on 2 fake pages with iBoot's CLCD state, and drop `function-lcd_enable`.
  - The kernel `beq` at 0x807A9B68 is NOP'd, so swap-done is synchronous.
  - liblockdown is stubbed in the dyld cache (no lockdownd).
  - `AppleKeyStoreGetDeviceLockState` is stubbed (it blocks without a system keybag).
- QuartzCore uses its software compositor (no AppleMBXDevice).
- Rendering is vsync-driven: the shim turns the 3DS PDC0 VBlank (GIC 42) into the S5L CLCD frame IRQ through the fake VIC.
- `tools/ios3ds-sample` prints every process's thread state to the console.
- **Result: SpringBoard draws the "Connect to iTunes" screen, mirrored onto the 3DS top screen by the ARM9.**

## 19. Input reaches SpringBoard (2026-11-16)
- Wired: ARM9 `input_poll` (pad + TSC) → ring at shim+0x6200 → sysctl `kern.ios3ds_input` (registered from the first `mdevstrategy` call) → `/usr/libexec/ios3ds-input`.
- QEMU: the daemon connects ("connected to SpringBoard's HID event system", "mailbox up: touch absent"; no TSC model yet) and logs `key 40 down/up` for START.
- Keyboard IOHIDEvents dispatched with `IOHIDEventSystemClientDispatchEvent` are **ignored** by SpringBoard: no visible effect for Home (0x40) or volume (0xE9).
- Buttons now go as GSEventRecords (52 bytes; `infoSize` at +0x30, checked in `_GSSendEvent` 0x352632c4) through `GSSendSystemEvent` (0x352635a4, resolved with dlsym) to PurpleSystemEventPort. **Delivered:** SELECT (lock 1010/1011) brings up "slide to power off" and an "Edit Home Screen" hint.
- **Open:** both are *hold* reactions. The down is honoured, but the up (sent 0.3 s later) is not, or not in time. Suspects: SpringBoard measures hold time against "now" while its run loop lags in the emulator; or the up needs extra record fields. Touch (GSHandInfo to the frontmost app) is still IOHID-only and untested (no TSC model).

## 20. Button timing in SpringBoard (2026-11-16)
- `-[SpringBoard lockButtonDown:]` (0x8a48) arms `lockButtonWasHeld` with a **2.5 s** NSTimer (`0x4004000000000000`). `lockButtonUp:` (0x9a2c) does nothing more once the power-down sheet `isOrderedFront`.
- `menuButtonUp:` (0x8db8) returns at 0x918e when no menu timer was set by the down. A tap then goes through the double-tap delay (`performSelector:_handleMenuButtonEvent afterDelay:`).
- **Reproducible:** a down sent immediately and an up 0.3 s later → "slide to power off" plus the "Edit Home Screen" hint (a *hold*).
- **Daemon now defers:** a tap sends down+up back to back on release; the down of a hold goes out after ~0.8 s (counted in poll ticks, because `mach_timebase_info` gave no usable value).
- With deferral, neither a lock tap nor a 5 s lock hold (tick-stamped: down @22836, up @22969) changes the screen. A Home tap on page 1 doesn't open Spotlight either. **Unresolved.**
- **Debug aids tried:**
  - `SBLogging` default: no output.
  - Setting SpringBoard's trace byte (0x18c264 → file 0x18b264): runs, but NSLog goes to ASL, and syslogd persists nothing (`/var/log/syslog` 0 bytes).
  - `CFLOG_FORCE_STDERR=1`: no visible output.
  - mkrootfs file patches now take an optional `mode`; they used to force 0644, which broke the patched SpringBoard executable.
- **Next:** get syslogd writing (or patch NSLog's ASL path to stderr), then trace which button handlers run.

## 21. Buttons work; the "holds" were the test harness (2026-11-16)
- `emu/run.py` turned every `key_down` into `sendkey <key> 3000` and ignored `key_up`. **Every test press was therefore a 3 s hold**, which explains the power-off sheet and the hint in entries 19/20.
- run.py now uses the down→up interval as the sendkey hold time.
- **Results:**
  - Home tap on page 1 → Spotlight ("Search iPod" with keyboard);
  - 4 s lock hold → "slide to power off";
  - lock tap → no visible change (screen-off isn't mirrored).
- The daemon's deferred-down for taps stays: a slow SpringBoard run loop can still lose a separate up to the 2.5 s hold timer on real hardware.
- The SpringBoard trace patch and syslog-to-console line were removed. NSLog never reached the console in any configuration we tried (open).

## 22. Touch works (2026-11-16)
- **QEMU:** CTR CODEC/TSC on NSPI bus 2 plus I2C/MCU, and an absolute pointer, all in `nintendo3ds.c` (from the QEMU agent's draft). The ARM9 driver reports `tsc_ok`, and pen samples land in the input ring with correct ADC values.
- **Ignored path:** digitizer IOHIDEvents dispatched by our daemon reach SpringBoard's event system but are ignored, as keyboard events were (entry 19).
- **Working path:** `kGSEventHand` (3001) GSEvents, layout from 8C148:
  - `_GSEventGetHandInfo` copies 0x24 bytes at record+0x34;
  - `_GSEventGetPathInfoAtIndex` = record+0x58+24*i;
  - `_UIApplicationHandleEvent` reads `pathInfosCount` at hand+0x21, treats type 8 as cancel, and treats a path as touching while proximity bit 1 is set.
  - Target: the frontmost app's purple port (`SBFrontmostApplicationDisplayIdentifier` → `GSCopyPurpleNamedPort`), else `GSSendSystemEvent` (SpringBoard).
- **Daemon:** needs `flipx` since the display blit rotates (`ix = rr*4/3`).
- **Result:** tapping Settings at iOS (47,320) opens Settings (`emu/tap.py`).
## 23. OpenGL ES 1.1: a software engine in MBXGLEngine's slot (2026-11-16)
- **Engine discovery** (OpenGLES `_eagl_init`, 0x34ff870c):
  1. `GLEngine.bundle` is tried first and is absent.
  2. `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))` must return ≥1 service.
  3. `CFBundleGetDataPointerForName(MBXGLEngine.bundle, "GLESGetEGLInterface")` is called and its result goes into `_mbx_dispatch`.
  - The MBX engine serves ES1 only (`initWithAPI:` checks `api == 1`).
- **Getting past the check:** one file patch on OpenGLES's own copy of the string (cache file 0x4ff9458) makes it read `"IOResources"`. The copies in MBX2D (0x30770c68) and MBXGLEngine (0x32ee0fac) are left alone.
- **Loading our engine:** `enable-dylibs-to-override-cache` makes dyld load our on-disk `MBXGLEngine` in place of the cache copy. The only `.dylib` files the rootfs keeps on disk are 12 symlinks into cache-only frameworks.
- **Engine table and call shapes:** read from every `_mbx_dispatch` call site in 8C148's OpenGLES. The table has 11 entries; 4.2.1 adds Set/GetProperty at +0x24/+0x28.
  - `CreateGC(sg, X+0x10, X+0xD34, X+0xC)` passes two 841-slot dispatch tables.
  - `attachImage:toCoreSurface:invertedRender:` calls `BindCoreSurface(gc, target, surface)`. It accepts `GL_RENDERBUFFER` only together with invertedRender=YES, and target 0x84F5 only with NO.
  - `swapNotification:` calls `SwapNotification(gc, IOMobileFramebufferGetID(fb), transaction, layer)`. The non-MBX path calls `IOMobileFramebufferSwapSignal(fb, transaction, layer)` instead.
- **CA drawable protocol:** read from the stock `_GLESBindView` (0x32eb4ba4), `_ViewTextureBeginIfNeeded` (0x32eb4a64), `_GLESPresentView` (0x32eb4628) and `_GLESCreateBuffer`/`_GLESDestroyBuffer`. The drawable is called through:
  - +4 `bind(d, 'BGRA'|'L565', {ctx, create, destroy})`
  - +8 `unbind`
  - +0xC `dequeue` → IOSurface
  - +0x10 `present(d, 1)`
- **The engine:** `tools/ios3ds-gl` is the CPU rasterizer behind a swappable `Backend`. The design is in docs/gpu.md. All 44 `gl*` imports of QuartzCore are among the 177 implemented entry points.
- **QEMU, run 1:**
  - The engine loads in `ios3ds-gltest` and in SpringBoard (`[gl] context created`, pid 8 and pid 16). SpringBoard creates an EAGL context even with `CA_ENABLE_OGL=0`. No unimplemented slot was hit.
  - Offscreen: all 10 pixel probes and `glGetError` pass (`gltest: PASS`).
  - On screen: drawing into the display's IOSurface through `attachImage:toCoreSurface:` runs at ~13.6 fps, including a 50 ms sleep per frame. The top LCD shows the scene: triangle, textures, blend, scissor bar and lit cube.
- **Regression:** with the demo off, the home screen stayed **black** (400/449 s). SpringBoard's context came from `-[CAWindowServer _detectDisplays]` → `M2CLCDDisplay::open` → `M2Display::M2Display` (0x346de5b8) → `Display::eagl_context(1)`.
  - The constructor probes ES2 and then ES1, and on success stores the API at `+0x17c`. API 1 picks 'L565' at `+0x180`.
  - Once a context exists, the display is GL-backed and the software compositor stops. Even with `CA_ENABLE_OGL=0`, no engine call followed, so nothing was drawn.
- **Fix:** a cache patch at file 0x46de620 (`bl eagl_context` → `mov r0,#0`) skips the ES1 probe.
- **Run 4:**
  - the home screen is back at 415 s;
  - the GL scene is drawn from ~425 s (449 s shot);
  - `gltest: PASS`;
  - SpringBoard creates no context.
- **Open:** CA's GL compositor needs L565 render targets plus the BindView/SwapNotification path, which RE covers but no run has exercised yet.
## 24. Audio: Apple's kexts on emulated PL080/I2S/I2C (2026-11-16)
- Kept `dmac0`/`i2c0`/`i2s0` on fake pages; see `docs/audio.md` for the design and addresses.
- `AppleARMIICDevice` transfer (0x802e4d1e, vtable 0x35c) takes `(this, op, sublen, sub*, len, buf*)`; op 1 = read, 2 = write (the CS42L58 driver's read-modify-write pairs in the boot log confirm it). Patched to a shim register file.
- `AppleARMPL080DMAC` filter (0x807d115c) reads the TC status once and writes IntTCClear once, so the shim can apply write-1-to-clear at its next service.
- **QEMU boot (450 s):** `AppleARMPL080DMAC::start`, `AppleS5L8900XI2SController::start`, three `_initDMAChannel`s, `AppleCS42L58Audio::start` and 78 I2C register accesses; SpringBoard home screen renders as before; mediaserverd idles with 19 threads.
- Volume-up (R) and lock (SELECT) presses reach SpringBoard (`[input] key e9/30`), but no HUD appeared and no output stream started, so the DMA/CSND path is covered only by the host test (`tests/test_audio.py`) so far.
- The ARM9 CODEC init reports "id 00" in QEMU (SPI stub) and carries on.
## 25. Real-hardware hang after "iBoot version": the clock base was 0 (2026-11-17)
- **Symptom (hardware runs 3–6):** silent stop right after `iBoot version: ios3ds-loader`; probes showed 34 clock reads (last one with CPSR `0x93`), one 10 ms deadline (caller `0x800695d3`), zero IRQ entries. Plain QEMU never hung.
- **Reproduction:** `-icount shift=2,sleep=off` (QEMU time = instruction count, ~250 MHz, roughly the Old 3DS ARM11) gives exactly the hardware probe values: `abs 34 cpsr 80000093 | dec 1 last 10473 caller 800695d3`. Without icount, QEMU's wall-clock timer runs far ahead of the slow TCG CPU: in a plain run clock reads 2–7 already come from IRQ mode (CPSR `0x192`), i.e. the first tick fires before read 2. `emu/run.py --icount 2` now does this.
- **Where it hangs** (`emu/gdb/postmortem.py`, attach after the hang): ARM11 at `0x80069186`–`0x80069198`, SVC, IRQs masked, in a loop
  `do { base = rtclock[+0x30..+0x34]; … } while (base == 0 || base changed)` (function `0x8006916c`, abstime → seconds/microseconds; `rtclock` = `0x80255128`). It is called from the calendar path at `0x80014eec`, between a lock/unlock pair (`0x80063420` / `0x800634c0`) that masks IRQs.
- **Who writes `rtclock+0x30`:**
  - `rtclock_init` at `0x8006993e`/`0x80069998`: the result of `mach_absolute_time()` (`bl 0x800694d4` at `0x80069936`);
  - the clock tick handler at `0x800696e4`: "now".
- **Cause:** `rtclock_init` runs before the first `set_decrementer`, and the shim was only initialised there. Until then `shim_get_abstime` returned **0**, so the base was 0. The spin then needed the first tick, which cannot arrive with IRQs masked. On Apple hardware the timer always runs; on QEMU the tick happened to come first.
- **Fix:** `shim_get_abstime` calls `shim_init` itself on the first read (stackless: r3/lr/CPSR parked in pc-relative words, IRQ/FIQ masked). In the icount run the kernel now goes on through IOKit, launchd and SpringBoard (`build/hwlogs/qemu-icount-fixed.txt`).
- **Not the cause (checked on the way):** the vector page and page tables. The new probe snapshot reads TTBR0/1, TTBCR, DACR, MPIDR, L1[0xFFF], L2[0xF0], the 8 vector words, the IRQ literal and the first two handler words from the ARM11 view; after a stall the ARM9 walks the same tables in FCRAM and compares. In the icount reproduction both views match (`e59ff018`…, IRQ literal `8006648c`, handler `e51ff004 8084001c`).
- **Tooling:** `emu/gdb/rsp.py` is a minimal GDB remote-protocol client. The Arm gdb 14.2 has no Python and the host gdb has no ARM target, so scripted traces go through this client: `reads.py` logs every clock read, `panic.py` breaks at `_panic`, `postmortem.py` attaches after a hang. The monitor writes all probe values to `log.txt` (`[probe]` lines) after ~5 s without a clock read or console output, so photos are no longer needed.
## 26. Real-hardware boot to SpringBoard, and boot-time performance (2026-11-17)
- **run8 (`9867d28`):** first hardware boot to SpringBoard. Touch, volume and Mail work (docs/hwlogs/run8.txt). It is slow: ~19 md0 requests/s, launchd at ~115 s, input at ~320 s.
- **Why md0 latency is the whole story:** `shim_mdevstrategy` (kernel/shim/blk.c) waits for each ≤32 KiB request with **IRQs off**. The ARM9 only answers between its other jobs. Those jobs were:
  - console output written one byte per f_write + f_sync;
  - a ~330k-pixel screen copy every 16 loop passes;
  - touch over SPI.
- **ARM9 PC samples** (`emu/pcsample9.py`, QEMU with `--icount 2`): 49% `send_command` (SD), 26% `display_blit`.
- **run9 (`763f374`):** chunked log with a 0.5 s sync; the block mailbox is polled inside the blit; one word read per VRAM pixel; screen copies paced (≤15 fps, 2/s while md0 is busy). SpringBoard and input come up in under 83 s (docs/hwlogs/run9.txt).
- **The 30 s boot wait** was `IOKitInitializeTime()` (0x801a8004, called from bsd_init at 0x8024a53e): `waitForService(resourceMatching("IORTC"), {30, 0})`. Nothing publishes IORTC. Found with a SELECT+START thread dump in QEMU during the wait: the bootstrap thread `0x80284664` was blocked under 0x801a8021. The earlier `rtc-wait-0s` patch only covered the later `getGMTTimeOfDay` wait. New patch `iortc-wait-0s` (`movs r3,#30` → `#0` at 0x801a800a). `BSD root` now follows immediately.
- **md0 linger:** after serving a request, the ARM9 polls the mailbox for up to 2 ms (at most 100 ms in total) before running the rest of its loop, so streamed reads go back to back. In QEMU with icount the emulated SD controller dominates, so this needs a hardware run to measure.
- **IORTC timeout 0 was too aggressive** (hardware run10 and one QEMU run): `gltest` measured `0 ms` per frame, so early userland `gettimeofday` deltas were frozen. With timeout 0, `clock_initialize_calendar()` can run before AppleARMPE (gIOPlatform) has registered. The "RTC did not show up" line is missing in those runs, which shows the platform expert's `getGMTTimeOfDay` was never called. The result looks like a race: a clean `--icount 2` run with timeout 0 measured 2486 ms. Patch changed to **2 s** (`iortc-wait-2s`, `movs r3,#2`). AppleARMPE is up within 1 s even in plain QEMU, so the original order is kept, while 28 of the 30 s are still saved. Clean `--icount 2` run: the RTC line is back, gltest takes 2531 ms, md0 starts at ~2 s, and boot reaches SpringBoard and input.
- **Lesson:** build releases from a clean worktree (`ios3ds-wt/rel`). Other agents' uncommitted edits in the main checkout (shim, input daemon) leaked into a test build and produced a broken boot (FS errors, `launch_msg(): Socket is not connected`) unrelated to the change under test.

## 27. PICA200 back end for the GL engine (2026-11-16)
Design and sources are in docs/pica200.md. This entry records what was checked and how.
- **Kernel facts for the GPU service** (`scripts/ksym_lib.py` over the kernelcache symbol table; the `n_desc & 8` bit says Thumb):
  - `copyin` 0x80067c84 and `copyout` 0x80067d04 are ARM. Disassembly shows `copyin(r0 = user address, r1 = kernel address, r2 = length)`, i.e. a 32-bit `user_addr_t`.
  - `IOMallocContiguous` 0x801a70e0, `ml_io_map` 0x80067780, `IODelay` 0x801a6c2c, `IOSleep` 0x801a6c40, `printf` 0x8001c4c4, `proc_selfpid` 0x8014f1be, `proc_find` 0x8014f94c, `proc_rele` 0x8014f862 are Thumb.
  - `_sysctl__children` (top-level OID list) is at 0x8027f7a8.
- **ABI:** iOS lets callees clobber r9; our shim C code is EABI and assumed r9 is preserved across calls into the kernel. The shim is now built with `-ffixed-r9` (`scripts/mkboot.py`). This also covers `blk.c` and the input service.
- **References fetched for semantics:** Azahar `src/video_core` (`renderer_software/sw_rasterizer.cpp`, `sw_clipper.cpp`, `sw_framebuffer.cpp`, `sw_blitter.cpp`, `sw_texturing.cpp`, `pica/pica_core.cpp`, `pica/regs_*.h`) from `raw.githubusercontent.com/azahar-emu/azahar/master`, and the local libn3ds copy (`gfx.c`, `gx.h`, `pdn.h`). Read, not copied.
- **Earlier draft reviewed** (`tools/ios3ds-pica` from the interrupted session): encoders, tiling, texenv mapping and the shader assembler were kept. Three things were wrong or missing and are fixed: the unit test expected a truncated f31 mantissa (the encoder was right); colour-buffer and PPF formats number RGB565/RGB5A1 differently (one enum was used for both); `GL_COMBINE` was not mapped. Its user-mapped register design was replaced by the shim service.
- **Host:**
  - `make -C tools/ios3ds-pica test` → `328 checks, 0 failures`. The last group renders one triangle through driver core + model and checks pixel positions with and without the transfer's flip flag.
  - `make -C tools/ios3ds-gl host-test` → 10 probes ok on both back ends; `scene 1, pica vs software: 27584 of 153600 pixels differ at all, 10 by more than 24`; `scene 2 … 39319 … 320 by more than 24`. Scene 2 (off-target viewport, NPOT texture, two units, alpha test, GL_COMBINE, masks, render-to-texture) found one back-end bug: with a viewport that does not fit, the stale GL scissor box was applied although the scissor test was off.
- **QEMU run 1** (node registered as `kern.ios3ds_gpu`): `[gl] pica: GPU not available: no kern.ios3ds_gpu service`, then `GPU pass skipped`; the software pass and the boot were unaffected. The errno was not logged in that run. The node was moved to the top level (`ios3ds_gpu`) and the transport now logs the failing step and errno.
- **QEMU run 2** (`python3 emu/run.py --time 160 --shots 150,159`, gltest arguments `45 300`):
  ```
  [pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT 0x00000000, GX clock 0x00000000, GX stat 0x00000000
  [pica] arena 8192 KiB at VA 0xcb114000 = PA 0x24684000; driver ready (state 0)
  [gl] pica: GPU up: arena 8192 KiB at PA 0x24684000 (8064 KiB free), PDN_GPU_CNT 0x1007f, PICA200
  [gl] context 0x904c00 uses the ios3ds PICA200
  gltest: GL_VERSION "OpenGL ES-CM 1.1 ios3ds", GL_RENDERER "ios3ds PICA200"
  (10 probes ok)
  gltest: pica vs software: 27584 of 153600 pixels differ at all, 10 by more than 24 (max 229) ok
  gltest: PICA200 PASS (0 failed)
  gltest: drawing into the display surface 320x480, status 0x8cd5
  ```
  The 150 s screenshot shows the animated scene on the top LCD, drawn through the emulated GPU into the display IOSurface. QEMU printed no `picasim:` diagnostic. (`GX clock 0` is the model; hardware should show 0x70100.)
- **QEMU run 3** (full boot, `--time 450 --shots 400,449`, gltest arguments `340 60`): the same `[pica]` / `PICA200 PASS` lines; the 400 s screenshot is the SpringBoard home screen as before, the 449 s one the GL scene drawn through the emulated GPU (10-11 fps including the 50 ms sleep per frame). No `picasim:` diagnostics, no `[pica] TIMEOUT`.
- **QEMU run 4** (committed configuration, gltest arguments `0 0`, with scene 2): `scene 1 … 27584 … 10 by more than 24 … ok`, `scene 2 … 39319 … 320 by more than 24 … ok`, `gltest: PICA200 PASS (0 failed)`: the same numbers as on the build host.
- **Not verified:** anything on a real PICA200. docs/pica200.md §5 says what the model can and cannot show, §8 lists what to look at on the first hardware run.

## 28. GPU display mirror; the PICA200 path reviewed for real hardware (2026-11-17)
Branch `agent/gpu` = `agent/pica` + main. Design: docs/pica200.md §9-11.
- **Why:** entry 26 measured 26% of the ARM9's time in `display_blit`, and the ARM11 kernel waits for md0 with IRQs off while the ARM9 is busy. The screen copy is the one big ARM9 job that a GPU can take.
- **The copy is a scaling.** In the LCD framebuffers' own layout (row j = screen column j, 240 pixels from the bottom up) display.c's rotation disappears: top = iOS (⌊4i/3⌋, ⌊4(j−20)/3⌋) for j 20..379, bottom = iOS (⌊(3i−40)/2⌋, ⌊3j/2⌋) for i 14..226. Written down once as `gm_map()` (boot/common/gpu_mirror.h).
- **Exact nearest sampling:** texcoord iOS x = 4/3·q − 1/2 at render-target column q puts every pixel centre 1/6, 1/2 or 5/6 into a texel, never near a boundary; same for the other three axes (3/2·q − 41/2, …). With truncating sampling (Citra, our model) that reproduces the integer maths exactly.
- **Colour without a BGRA texture format:** iOS bytes B,G,R,A read as RGBA8 (A,B,G,R) give sample.g = R, .b = G, .a = B; three TEV stages with constant colours (1,0,0), (0,1,0), (0,0,1) and MODULATE / MULT_ADD rebuild (R,G,B) exactly.
- **Host test** (`make -C tools/ios3ds-pica test`, `test_mirror`): boot/arm9/display.c compiled for the host (VRAM mapped at 0x18000000 with MAP_FIXED so its 32-bit addresses work) against the engine + model: `mode top/both/bottom: GPU mirror vs ARM9 CPU copy (display.c): 0 of 1566976 bytes differ`, also with a GPU that finishes operations only several polls later (`3 frames needed 72 busy steps`). The self-test catches a deliberately wrong list (`249666 bad bytes … caught`) and an unreachable list times out at the P3D stage.
- **First QEMU boot** (plain, `--time 450`): self-test variant 0 exact, mirror on, home screen on both LCDs. But the model ran each operation inside the MMIO write, so the 52 ms model draw ran inside the VBlank handler: `ARM11 IRQ time 55055 us/frame` — a QEMU artefact, and the multi-interrupt stepping was never exercised. **Fix:** the QEMU GX model now holds back the starting write in virtual time (PSC 100 µs, PPF 400 µs, P3D 1 ms) and runs it from a timer.
- **QEMU, plain, async model** (`--time 450`, X at 405/420/435 s): `[gpu] mirror self-test variant 0 (as modelled): 0 of 518400 bytes differ, guard intact`, ~10 fps (`frame avg ~75000 us`, 6 interrupt steps per frame, `ARM11 IRQ time 129-529 us/frame`), `spot check (GPU copy): 0 of 512 sampled LCD pixels differ` every 30 s, `gltest: PICA200 PASS (0 failed)` while the mirror runs (`3 busy skips`), all three screen modes correct with the console redrawn on the screen given back (build/emu-plain2/screen_415/430/449).
- **QEMU, `--icount 2`** (`--time 300`): CPU copy before the mirror starts: `44 copies in 30 s, 161.5 ms each`, later `103 copies, 56.0 ms each` (ARM9, emulated timing); once the mirror is on (after the GL test), `671 frames in 30 s (22.3 fps)`, spot checks 0/512, `gltest: PICA200 offscreen frame took 1088 ms` vs `2098 ms` software. The GPU stage times in QEMU are the model's delays plus interrupt latency, not hardware figures.
- **Real-hardware review of the existing GPU service** (docs/pica200.md §10): the arena came from `IOMallocContiguous` with whatever dirty lines its previous owner left: a write-back of one of them could land on GPU output at any later time. Now the whole D-cache is cleaned and invalidated once after allocation. boot.cfg `gpu=0` stops bring-up before any register access. The GL engine checks a PPF round trip and a PSC fill through the service before `pica`/`auto` use the GPU, and logs what came back if not. No GIC line was added: completion stays polled, the mirror is stepped from the existing VBlank (42) and timer (29) interrupts.
- **Shim size:** with the mirror the shim ended at +0x741C, past the +0x5F00 limit (probe snapshot). The GPU code (pica.c, pica_drv.c, pica_mirror.c, 16 KiB incl. bss) is now linked at 0x808B0000 and loaded from `gpu.bin` (with the command lists at 0x208B8000); the main shim shrank to 0x3414 and calls the part only through `gpu_glue.c` when its magic is present. The ARM9 clears that magic before the `load=` entries so a stale copy from a previous boot in FCRAM is never called.
- **CPU copy change:** a screen that starts showing iOS is cleared first. Before, the top screen's 20-column side margins kept console text in "both"/"top" mode; the GPU writes them black, and so does the CPU copy now.
- **Not verified on hardware:** everything in this entry. docs/pica200.md §11 lists the log lines that settle each assumption.
- **GPU vs CPU screenshots:** the same build with `gpu_mirror=0` (`[gpu] display mirror disabled (boot.cfg)`, `CPU screen copy: 109 copies in 30 s, 36.7 ms each`): its 400 s and 449 s screenshots are pixel-identical to the GPU-mirror run's (0 of 192000 pixels differ, both screens, "both" mode). Touch is unaffected: the input daemon's panel → iOS mapping (`flipx`, 2/3 scale) was never touched and the picture under the finger is the same.
- **Fallback, injected:** the final build with gpu.bin's list 0 replaced by list 1 (wrong sampling for the model): all 8 variants logged with their first difference, then `[gpu] display mirror FAILED (self-test mismatch): 249666 bytes differed, first at 14403: 01 expected, 00 seen; the ARM9 keeps copying the screen`; CPU copy continues (`spot check (CPU copy): 0 of 512`), gltest still `PICA200 PASS`, and the 399 s screenshot equals the CPU-only run's. The unmodified final build again: variant 0 exact, mirror on, 449 s screenshot identical to the CPU-only run's.

## 29. Swipes never ended: the shim starved vsync (2026-11-17)
- **Symptom (QEMU, viewer and `emu/tap.py --drag`):** after a fast swipe the home-screen page froze half-dragged. Every later touch moved it again and froze again; a tap left the Spotlight "W" key popup up.
- **Not the touch path:** the daemon logs a down and an up for every gesture. GraphicsServices backlogs full-queue sends and re-sends them with a blocking `mach_msg` (`_GSSendEvent` 0x352632c4, thread 0x352638a8). UIKit ends a touch on hand type 6 with proximity bit 1 clear (`_UIApplicationHandleEvent` 0x3381ecf4). Pacing and coalescing the daemon's events changed nothing.
- **Cause:** UIScrollView pages and bounces through `UIViewHeartbeat` → `GSHeartbeat`. `GSHeartbeatCreate` (0x35264a6c) ticks on `IOMobileFramebufferGetVSyncRunLoopSource`, i.e. CLCD frame interrupts. AppleM2CLCD keeps its frame-IRQ enable (+0x08 bit 0, shadow ivar 0x254) set while anyone wants frames, and clears it itself when idle (interrupt action 0x807a9df8). The shim cleared it after every injected VBlank ("one-shot"), and the driver, trusting its shadow, never set it again until the next swap. Once the finger lifted nothing swapped, so the heartbeat never ticked.
- **Fix:** the shim leaves the enable alone and injects on every VBlank while it is set.
- **Result (`emu/tap.py --drag 450:280,240:40,240 --drag 462:40,240:280,240 --tap 475:47,320`):** the page snaps back after the swipe and the tap opens Settings. A 240 pt right swipe in 0.4 s did not page to Spotlight (open).
- `emu/viewer.py`: a mouse release outside the views used to be dropped (pen stuck down); drags now stay in the view they started in and always end. `emu/tap.py` gained `--drag`.

## 29b. Hardware run 12: the PICA200 does not fill or draw; GX clock was never set (2026-11-17)
- **run12** (`m1-21f26fc`, docs/hwlogs/run12.txt): mirror self-test wrong in every variant (first byte 0x80, 0x40 with the read-out flip; ~451k of 518k bytes), GL self-check: `memory fill left 4096 of 4096 words wrong (word 0: 0x5a5aa5a5)`. Fallbacks fine. ARM9 CPU copy: 313-433 ms per frame.
- **Reading the failure:** the GL self-check's transfer round trip passed first, so PPF, addresses (PA>>3) and the cache maintenance work. The fill left the old data, yet its done bit came on. The mirror's read-out produced correct black margins (first 14400 bytes), so its last stage ran; the render targets held wrong data.
- **GX clock:** `GX clock 0x00000000`. Compared the register sequences with libn3ds (`source/arm11/drivers/gfx.c hardwareReset`, `include/arm11/drivers/gx.h`) and Luma (`arm11/source/main.c initScreens`): libn3ds writes `gpu_clk = 0x70100`, clears the VRAM bank-disable bits (+0x030 bits 8-11), sets `psc_dma_prio0/1` (+0x050/+0x054); Luma writes none of them, only PDN_GPU_CNT and the PDC registers. Our bring-up skipped them because "Luma leaves the GPU running" was only checked through PDN_GPU_CNT.
- **Also from libn3ds gx.h:** GX +0x70..+0xBC are memory-traffic counters (PSC external/VRAM reads and writes, P3D vertex/texture/depth/colour, PPF, fills). They show whether an engine touched memory at all, independent of done bits.
- **Changes:** bring-up applies the clock/VRAM/priority setup (boot.cfg `gpu_clk`, default 1). `pica_diag.c` runs staged tests before and after it (fills to VRAM and FCRAM; PPF; P3D constant quad into VRAM and FCRAM; 64x64 and 48x40 textures drawn 1:1) with counter deltas (boot.cfg `gpu_diag`, default 1); docs/pica200.md §12 says how to read them. A failed mirror self-test also dumps a texture tile and a render-target tile with their expected values. The diagnostic lists are built by genmirror (blob version 2).
- **Host:** the diagnostics pass on the model (`diagnostics on the model: 0x7f`); the dump's expectations equal what a passing self-test leaves (`16 of 16`). The model has no traffic counters (they read 0) and does not need the clock register.
- **QEMU** (this build, `--time 450` and `--icount 2 --time 300`): `[pica] GX setup as libn3ds hardwareReset: clock 0x00000000 -> 0x00070100 …`, `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM ok->ok …; PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT ok` (the model fills and draws without the clock, so "as found" passes there), mirror self-test variant 0, `gltest: PICA200 PASS`, home screen; screenshots identical to the CPU-copy run in both modes; icount mirror 23.9 fps, spot checks 0/512.

## 30. Run 13's answer: no FCRAM fills, no NPOT textures; the model now agrees; fixes (2026-11-18)
- **run13** (`eab9618`, docs/hwlogs/run13.txt): `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM FAIL->FAIL; PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT FAIL`. Counters for the FCRAM fill: `fill0 +256 ext-wr +0` (the engine ran, nothing reached external memory); NPOT 48x40: 0/1920 texels, `tex +256`. The GX clock setup changed nothing (VRAM fills worked before it).
- **Texture tile dump explained** (run13 lines 288-291): decoding the words with the self-test pattern gives iOS pixels (0,479) (1,479) (0,478) (1,478) (2,479) …: a correct linear→tiled upload done *with* the flip flag, i.e. the last variant tried (7). The target tile holds (128..130, 300) throughout: NPOT sampling. The dump now re-runs variant 0 first.
- **Model = hardware:** picasim writes nothing for fills outside VRAM and samples noise from NPOT textures. Proof that it reproduces the hardware: the previous build (c0ee62e) on the new QEMU gives the run13 summary line verbatim, `[gpu] display mirror FAILED (self-test mismatch)` and `[gl] pica: GPU self-check: memory fill left 4096 of 4096 words wrong (word 0: 0x5a5aa5a5)`, the exact hardware message (build/emu-hwmodel); the host tests failed the same way before the fixes.
- **Fixes:** mirror texture 512x512 in the FCRAM arena, filled by a PPF texture copy (60 tile rows of 10240 bytes, output gap 6144) from the 320-wide upload in VRAM; GL clears as P3D quads, depth initialised by CPU write, self-check draws a quad. docs/pica200.md §13.
- **CPU copy** (docs/pica200.md §14): row reads and column writes as 16-byte LDM/STM bursts, 4 pixels packed into 3 words, only the framebuffer set the PDC shows (the shim publishes PDC0/1 +0x78; QEMU: `LCDs show framebuffer set 0 on top, 0 on the bottom`). Host test: byte-identical to the GPU model and to the old copy's mapping.
- **QEMU `--icount 2`, CPU copy only (`gpu_mirror=0`), same boot phases, old (c0ee62e) → new:** 126.4 → 48.1 ms, 181.0 → 54.6 ms, 194.3 → 65.3 ms, 51.5 → 14.4 ms per copy (instruction-clocked: fewer instructions; the hardware gain from bursts and from writing one set instead of two plus a read-back is not visible in QEMU). Screenshots of the three icount runs differ only in the status-bar clock.
- **QEMU with the fixes:** plain 450 s and `--icount 2` 300 s: diagnostics summary as on hardware, `[gpu] display mirror: self-test passed (variant 0)`, `screen copy: PICA200`, `[gl] pica: GPU up (self-check passed)`, `gltest: PICA200 PASS (0 failed)`, spot checks 0/512; icount mirror ~20 fps. Plain 449 s screenshot identical to the CPU-copy run's.
- **Open on hardware:** the PPF texture copy with an output gap and texture fetches from FCRAM. Both are covered by the mirror self-test (failure → CPU copy, logged with a dump).

## 31. Stability soak; md0 no longer freezes the kernel (2026-11-18)
- **`emu/soak.py`:** boots QEMU, waits for `[input] connected to SpringBoard`, then cycles through the page-1 icon grid and the dock:
  - per cycle: tap, settle, Home, page swipes every 4th cycle, lock/wake/slide-to-unlock every 5th, a screenshot per step;
  - recovery when a cycle doesn't end on the home screen (second Home, then Dismiss), recorded per cycle;
  - afterwards the log is scanned for panics, `[probe]` stalls, md0/log errors, crashes, jetsam and SpringBoard restarts.

  Output: `build/emu/soak*/report.md`.
- **Soak 1** (main `15f2a30`, plain QEMU, 40 cycles, ~19 min): no panic, stall, I/O error, crash or memory warning, and SpringBoard never restarted.
  - Cycles 13 and 33 "failed" only because the tap missed an icon and Home on page 1 opens Spotlight (correct iOS behaviour). Hence the recovery step.
  - **Real bug, cycle 0:** the first app launch after boot (Game Center). The input daemon sent Home as down+up in the same poll (`key 40 down @1644` / `up @1644`), yet SpringBoard showed the "Edit Home Screen" alert, its reaction to a Home *hold*. The alert then blocked every tap for ~5 minutes.

    It did not reproduce in 8 quiet Game Center cycles (`--targets 47,50 --settles 2,20`). It needs the busy first launch.
- **Cause (by design reading):** `shim_mdevstrategy` waited for each md0 request with **IRQs off**:
  - ~20 ms per request on hardware, thousands of requests for a cold app launch;
  - meanwhile no timer, scheduler or VBlank ran anywhere in the kernel.

  If SpringBoard handled the Home down, then page-faulted its own code from md0 before handling the up, its hold timer (`menuButtonDown:` → `menuButtonUp:`, re-log 20) could expire during the freeze.
- **Fix (`kernel/shim/blk.c`):**
  - md0 requests are serialized by a kernel mutex. The XNU KPIs come from the kernelcache symbol table:
    - `lck_grp_attr_alloc_init` 0x8001ae22, `lck_grp_alloc_init` 0x8001ae00, `lck_attr_alloc_init` 0x8001ade8, `lck_mtx_alloc_init` 0x80062008 (Thumb);
    - `lck_mtx_lock` 0x80063000 and `lck_mtx_unlock` 0x800631a0 (ARM);
    - `assert_wait_timeout` 0x8001f02a and `thread_block` 0x8001edcc (Thumb).
  - The wait for the ARM9 runs with interrupts on: it spins ~200 polls, then sleeps in 250 µs steps (`assert_wait_timeout` + `thread_block`).
  - The whole buffer is faulted in **before** taking the lock, so no fault ever waits on md0 behind its own request.
  - If the lock can't be allocated, the old IRQs-off path is used.
- **QEMU `--icount 2`:** by the first heartbeat (t=36 s) md0 had served 3687 requests (78 MB), with launchd, gltest and the input daemon already up. Before: ~1100 requests and 32 MB by t≈120 s.
  - Much of that gain is an emulation effect. Single-threaded icount TCG shares one virtual clock between the CPUs, so the spinning ARM11 used to burn the time the ARM9 needed.
  - On hardware the gain is responsiveness: other threads, timers, VBlank and input keep running during disk reads.
- **Soak 2** (this change, 40 cycles):
  - **PASS:** no panic, stall, md0/log error, crash or jetsam; ~9,400 md0 requests served through the new lock/sleep path.
  - Spotlight cases (13, 14, 33, 34) recovered with a second Home.
  - The "Edit Home Screen" alert came back once, again on cycle 0, the first app launch after boot. So the IRQs-off freeze was not the cause.
- **Correction (soak 4, §39):** the alert is **not** a hold. It is SpringBoard's one-time `REORDER_INFO` tip, remembered in the default `SBDidShowReorderText`, which a fresh rootfs (every QEMU soak) doesn't have. The timer analysis below is still accurate, but it wasn't what happened here.
- **Why a Home tap can become a hold** (`SpringBoard` `-[SpringBoard menuButtonDown:]` 0x8bc0, `_menuButtonWasHeld` 0x8874, found through the objc method lists):
  - The down schedules `scheduledTimerWithTimeInterval:… _menuButtonWasHeld` with one of three intervals:
    - **2.0 s** normally;
    - **1.0 s** when a SpringBoard flag is set and `allowAllInBackground` (0x3ff00000);
    - **5.0 s** while a sheet `isOrderedFront` (0x40140000).
  - `menuButtonUp:` cancels it. The daemon queues down and up together, so a hold only happens when SpringBoard needs more than 1–2 s between handling the two GSEvents. That happens on the busy first app launch after boot.
  - The remedy is a faster SpringBoard (GPU compositing, re-log 30 / agent/gpu), not the input path.
  - `emu/soak.py` allows one such "Dismiss + Home" recovery per run and fails on more, or on any "stuck" cycle.

## 32. Audio: "could not start DMA: device is not ready", fixed; PCM reaches CSND (2026-11-18)
- **Symptom (hardware runs 8-11):** each UI sound (a tap, the Home key) logged `AppleEmbeddedAudioDevice: could not start DMA: device is not ready` and no stream ever started. Reproduced in QEMU: two Home presses, two errors (agent/audio2 `build/evidence/run1-before.txt`).
- **Where the message comes from:**
  - The format string `%s: could not start DMA: %s` is in AppleARMIISAudio (0x80612e0c), printed by `startDMA` (0x80611b48) when 0x80611584 fails. The text "device is not ready" is IOKit's string for kIOReturnNotReady (0xE00002D8).
  - The call chain is 0x80611474 → 0x806113c8 → I2S controller vtable 0x358.
- **The PL080 is not the culprit:**
  - `startDMACommand` (vtable 0x354, 0x807d00e4) does return NotReady for channel state ≤ 1.
  - But `_initDMAChannel` leaves channels at state 2, and the boot log shows channels 3, 4 and 5 initialised.
  - State map: 1 = allocated, 2 = ready, 3 = running.
- **The I2S controller's DMA start (AppleS5L8900X kext, 0x8066d448):**
  - For `i2s-version` 0 (the DT has no such property, so the boot log reads "i2sVersion: 0") with frame sync requested, it sets state 1 (0x8066d490), enables its interrupt and arms a timeout.
  - It then sleeps on the state.
  - The interrupt action (0x8066caf0) moves the state 1 → 2 on one edge and 2 → 3 on the next. The timeout (0x8066cb84) sets 4.
  - Anything but 3 returns NotReady; a timeout returns 0xE00002EB.
- **That interrupt is GPIO pin 44:**
  - i2s0's `interrupts` is the string `','` = 0x2c, and its `interrupt-parent` 0x0090c9a0 is `/arm-io/gpio`, not the VIC.
  - So the driver waits for two edges of a pin (presumably the word clock) to start DMA on a frame boundary.
  - The 3DS has no such pin, and the GPIO block is a fake page.
- **Fix:** kernel patch `i2s-start-no-lrck-sync` (`kernel/patches-audio.json`): `mov r3,#1` → `mov r3,#3` at 0x8066d490.
  - The wait loop falls through as if synced. The interrupt is disabled and the timeout cancelled as on success.
  - A DT `i2s-version` ≥ 3 would also skip the wait, but it changes register and clock programming. Not taken.
- **QEMU CSND model** (`emu/qemu/hw/arm/ctr_csnd.c`): plays channels from FCRAM on the virtual clock, ST_ADDR reads the play position, and a stereo mix sink logs `[csnd]` statistics and writes a WAV with `IOS3DS_CSND_WAV`.
- **Test client:** `ios3ds-tone` (AudioQueue via dlopen; left = tone, right = half amplitude).
- **Run 2 (plain TCG):** no more NotReady. The tone stream started (`tx start ch5`) and CSND played L/R at the exact amplitudes, but only 1,914 of 17,640 frames were tone.
  - The fragments are whole 1,024-frame AudioQueue buffers: underruns.
  - QEMU's guest runs about 3x slower than the 3DS (home screen at ~200 s vs ~70 s) while the audio clock is real time.
- **Run 3 (`--icount 2`):** after the first seconds of boot, every 1 s tone is 44,099 frames, gap-free, 999 Hz, peak 5999 L / 2999 R: bit-exact.
- **Volume is not applied in the PCM:**
  - Volume keys change neither the samples nor the codec level.
  - Each press writes one byte to the LM48821 speaker amp: `ab a3 9b 8b 83` down, `8b … c3` up.
  - AppleLM48821Amp setGain (0x807b8108) maps whole dB to a 5-bit step (19 = 0 dB, 31 = +18 dB) and writes `step << 3 | 3`.
  - Its mute (0x807b8048) writes `0x04`; its range is −38..+18 dB (0x807b800c).
  - Run 4: volume-up stops at step 31, volume-down goes 31 … 11 and then mute (`0x04`, plus CS42L58 regs 6 = 0, 0x1a = 0x80).
- **Shim volume mapping:** a step becomes the CSND channel volume `10^((dB − 18)/20)` (step 31 = unity) and `0x04` becomes 0.
  - It is applied to the running channels at the next service.
  - Run 4's recording follows it exactly: step 16 → peak 3007 = 6000 × 16422/32768; step 11 → 239.
- **Headphones:**
  - iOS stays on its speaker route (no `function-hp_detect`), so its volume always arrives as amp steps.
  - The CSND volume is digital, so it applies to either 3DS output.
  - The ARM9 now polls the jack (GPIO2 bit 0, debounced) and repeats the init's `HEADSET_SEL` routing on a change.
- **Run 5 (final code, `--icount 2`, 85 tones):**
  - Home pressed twice: 0 NotReady errors.
  - Volume swept up to step 31, down to mute and back up. Every recorded tone plays at exactly dB(step) − 18: step 22 → −12.0 dB, step 31 → 0.0 dB, step 11 → −34.0 dB.
  - While muted, 28.2 s of digital silence although tones kept playing; the level returned on the next step.
  - The ARM9 logs `[audio] iOS volume: amp step N -> CSND volume V/32768`.
- **Hardware safety:** the patch only skips a wait for a pin the 3DS does not have. The volume path only writes CSND channel VOL. The tone client logs and exits on any AudioToolbox error. Jack polling only runs when the CODEC answered at boot.

## 33. RTC: the calendar from the 3DS MCU (2026-11-18)
Tools: the flat kernel image as an ELF (`llvm-objcopy -I binary -O elf32-littlearm --change-addresses 0x80001000`), `llvm-objdump-18 --triple=thumbv6k-none-eabi`, the LC_SYMTAB dump (`scripts/ksym.py`).
- **Who asks for the time.** `IOKitInitializeTime` (0x801a8004, from bsd_init) = `waitForService(resourceMatching("IORTC"), {t, 0})` (literals 0x801a8028 `resourceMatching` 0x801ae02d, 0x801a8030 `waitForService` 0x801b18f1), then `clock_initialize_calendar` (0x80014ba0, literal 0x801a8034).
- **`clock_initialize_calendar`** (0x80014ba0): `secs = PEGetGMTTimeOfDay()` (bl 0x801c84d8), `commpage_set_timestamp(0, …)` (0x8006ade0, see §31), boot time at 0x80283e40 set once, `clock_get_system_microtime` (0x80069818), epoch at 0x80283dc8 = secs − uptime, offset via `nanoseconds_to_absolutetime`, adjustments cleared (0x80283de8), then `host_notify_calendar_change` (0x80016dcc).
- **`PEGetGMTTimeOfDay`** (0x801c84d8, Thumb): `gIOPlatform` (0x8029d490) ? vtable +0x390 : 0. `PESetGMTTimeOfDay` (0x801c84f4): vtable +0x394. AppleARMPE's vtable (pointer to it + 8 = 0x802f483c): +0x390 = `getGMTTimeOfDay` 0x802e78ad (found by searching the image for that word: one hit, 0x802f4bcc), +0x394 = `setGMTTimeOfDay` 0x802e7835, +0x388 = haltRestart 0x801c8645 (IOPlatformExpert's).
- **AppleARMPE::getGMTTimeOfDay** (0x802e78ac): `waitForService(resourceMatching("IORTC"), {30 s})` (the `rtc-wait-0s` patch), `getProperty("IORTC")` (vtable +0x9c), `OSDynamicCast(IOService)`, then `callPlatformFunction("getGMTTimeOfDay", false, &t, 0, 0, 0)` (vtable +0x1f8). So iOS expects a PMU driver to publish an `IORTC` resource whose value answers that platform function. **setGMTTimeOfDay** (0x802e7834) has the same 30 s wait and was never patched: a `settimeofday()` would have blocked for 30 s.
- **Chosen design:** not a fake IORTC provider (a C++ object with a hand-made vtable in the shim), but the two C entry points: `PEGetGMTTimeOfDay` → `shim_mcu_get_gmt` and `PESetGMTTimeOfDay` → `shim_mcu_set_gmt` (Thumb `ldr r3,[pc,#0]; bx r3; .word`, as for mdevstrategy). The time comes from the ARM9's MCU mailbox (docs/mcu.md), filled before XNU starts. Without a valid RTC the shim runs the original body, so the fallback is unchanged.
- **The IORTC wait:** the literal 0x801a8030 now points at `shim_mcu_wait_rtc`. With an RTC it releases the matching dictionary (vtable +0x14) and returns at once; else it calls `waitForService` with the 2 s of `iortc-wait-2s`. The reason for the 2 s (§26: calendar before AppleARMPE) is gone: the calendar no longer goes through the platform expert. The "0 ms" symptom it was blamed for was something else (§31).
- **QEMU** (plain, `build/emu-r2`): `[mcu] version 13/38: RTC 2026-11-17 23:21:31 Tue (offset 0 s) = Unix 1794957691`, then `ios3ds-mcu: 3DS RTC present: not waiting for IORTC`, `ios3ds-mcu: calendar from the 3DS RTC: 1794957691`; no "RTC did not show up" line any more. SpringBoard shows 3:26 PM and the calendar icon "Tuesday 17" (host clock 23:26 UTC, iOS time zone US/Pacific).

## 34. gettimeofday() froze for up to a second: the commpage reciprocal (2026-11-18)
- **Symptom:** gltest's first QEMU run with the RTC: `gettimeofday … tv 1794957716.452931 -> 1794957716.452931; mach_absolute_time 221564 ticks = 212 ms`, i.e. "frame took 0 ms" again although the calendar was now initialised early with a real time. So neither the IORTC wait nor `secs = 0` was the cause (§26).
- **Userland:** `gettimeofday` (libSystem, dyld cache 0x32ca92c4) calls `__commpage_gettimeofday` (0x32d4b748) first and only falls back to the syscall when it returns non-zero. That reads the commpage at 0x40000040: base abstime (+0), seconds (+8), microseconds (+0xc), limit (+0x10), divisor (+0x14), reciprocal M (+0x18), "add" flag (+0x20), shift (+0x24). Base 0 or `now - base >= limit` → syscall; else `usec += q(now - base)` with `q = umulhi(d, M)`, `add ? ((d - q) >> 1 + q) >> (shift - 1) : q >> shift`.
- **Kernel:** `commpage_set_timestamp` (0x8006ade0) recomputes M/add/shift (Hacker's Delight "magicu") whenever the divisor changes. Its caller `clock_gettimeofday_set_commpage` (0x8006948c) passes `rtclock+0` (ticks per second) as the limit and `rtclock+4` (ticks per µs) as the divisor. Read from QEMU (`xp /16wx 0x20255128`): `0x000ffb0f 0x00000001 0x3b9aca00 0x000ffb0f` = 1047311 ticks/s, **divisor 1**, timebase 10^9/1047311.
- **Divisor 1:** the algorithm overflows: M = 0, add = 1, shift = 0 (Python transcription; for 2 or 24 it is fine). Confirmed in QEMU (`emu/tap.py --hmp "245:x /12wx 0x40000040"`): `0x0f012b4f 0x00000000 0x6afce4c6 0x0002ba6f | 0x000ffb0f 0x00000001 0 0 | 0x00000001 0`. Userland then shifts by −1, i.e. by 255: elapsed time is always 0. gettimeofday returns the value of the last syscall refresh until a second of ticks has passed. Every earlier symptom fits: "0 ms" frames, hardware run12/13 `1063 ms` vs `mach_absolute_time 412 ms` (the first sample was ~650 ms stale), and the QEMU differences between the 2 s and 0 s IORTC waits (just where the frame fell relative to a refresh).
- **Fix** (`commpage-gtod-off`, kernel/patches-mcu.json): the `beq` at 0x8006adfc that skips the update when there is no commpage becomes `b`, so the timestamp is never published, the base stays 0 and userland always uses the (exact) syscall. Cost: one syscall per gettimeofday.
- **After the fix:** plain QEMU `gltest: timing: … tv 1794958858.470788 -> 1794958858.651883; mach_absolute_time 190712 ticks = 182 ms` (181 vs 182 ms); `--icount 2`: 2254 ms both. ios3ds-mcud's cross-check: `RTC +118 s, gettimeofday +117610 ms, mach_absolute_time +117613 ms` (plain), `RTC +153 s, gettimeofday +154086 ms, mach_absolute_time +154084 ms` (icount, `-rtc clock=vm`).
- **mach_absolute_time is right:** the timebase is consistent (10^9 / 1047311), and the shim's clock follows the MPCore watchdog.
- **Still inexact, but bounded:** the kernel's own microtime path (0x8006916c) also adds raw ticks as microseconds inside a one-second window (`rtclock+0x30` base): up to 4.7 % fast for under a second, corrected at the next full conversion.

## 35. Battery: an IOPMPowerSource from the shim (2026-11-18)
- **What SpringBoard reads:** `strings` on SpringBoard (from the rootfs with `build/tools/hfsplus … cat`) shows `IOPMPowerSource`, `CurrentCapacity`, `MaxCapacity`, `ExternalConnected`, `FullyCharged`, `IsCharging`, `ExternalChargeCapable`, `AppleRawBatteryVoltage`, `kIOPMPSAtCriticalLevelKey` ("Shutting down immediately …"); its imports (`arm-apple-darwin10-nm -u`) include `IOServiceGetMatchingService`, `IOServiceMatching`, `IORegistryEntryCreateCFProperty`. The kernelcache has no battery driver the 3DS could use (the n72 PMU is a D1759 on I2C, deleted from our device tree).
- **Kernel KPI (LC_SYMTAB):** `IOPMPowerSource::powerSource()` 0x801bb624 (`new` 0xa8 bytes + `init()` via vtable +0x44), setters `setCurrentCapacity` 0x801bb3fc, `setMaxCapacity` 0x801bb3d0, `setIsCharging` 0x801bb1f8, `setExternalConnected` 0x801bb288, `setExternalChargeCapable` 0x801bb258, `setBatteryInstalled` 0x801bb228, `setVoltage` 0x801bb348, `setPSProperty` 0x801bb0dc, `updateStatus` 0x801bb428 (copies the PS dictionary at +0x54 into the registry with `setProperty` (vtable +0x6c) when +0x50 is set, then `messageClients(0xe0024100)` = kIOPMMessageBatteryStatusHasChanged, vtable +0x238); `IOService::attach` 0x801adc54, `registerService` 0x801b0844, `getPlatform` 0x801ac744; `OSSymbol::withCStringNoCopy` 0x8019a5d0, `OSNumber::withNumber(u64, bits)` 0x80197684; `release` = vtable +0x14.
- **Gotcha:** `kOSBooleanTrue` / `kOSBooleanFalse` (0x80232e4c / 0x80232e50) are C++ *references*: the words hold `&gOSBooleanTrue` (0x8029d168). Passing the word itself made `OSDictionary::setObject` call through a vtable at 0: `sleh_abort: prefetch abort in kernel mode: fault_addr=0x0`, `lr 0x8018e521` (= `OSDictionary::setObject+0x68`), r0 0x8029d168 (QEMU run 1). Fixed by dereferencing twice.
- **QEMU** (`build/emu-r2`, `build/emu-r3`): `ios3ds-mcu: battery 100%, charger, 4200 mV: IOPMPowerSource registered`; the status bar shows a full battery with the charging bolt; after `qom-set /machine mcu-charging off` + `mcu-battery 50` a half-full battery without the bolt; with `mcu-battery 15` SpringBoard's own "Low Battery — 20% of battery remaining" alert.

## 36. HOME, POWER, shell and shutdown (2026-11-18)
- **Input path:** `input_proto.h` already reserved `INBTN_HOME` / `INBTN_POWER`, and ios3ds-input maps them to the Menu (0x40) and Power (0x30) usages. The ARM9 now ORs the MCU state into each input sample (`mcu_buttons()` in `input_poll`).
- **MCU IRQs** (regs 0x10–0x13, clear on read; mask 0x18–0x1B, 1 = masked; libn3ds `mcu.h`): bit 0 POWER press, 1 POWER held, 2/3 HOME press/release, 5/6 shell close/open. POWER has no release IRQ, hence the tap/long-hold scheme (docs/mcu.md §3).
- **Halt:** `PEHaltRestart` (0x801c98b0) waits (30 s deadline, 0x801c98d2) for the shutdown handlers for types 0/1/3, then `gIOPlatform->haltRestart(type)` (vtable +0x388 → IOPlatformExpert's 0x801c8645). The call site at 0x801c98f6 becomes `adds r1,r5,#0; ldr r3,[pc,#0]; b +2; .word shim+0x38`, keeping the original `blx r3` at 0x801c9900; the shim forwards every other type to the platform.
- **Shutdown hang:** sliding "slide to power off" logged `launchd: System: About to call: reboot(RB_HALT).`, then the CPU idled forever (heartbeat PCs all 0x80067b34, the idle WFI). SELECT+START dump, launchd's thread (annotated with `scripts/btmap.py`): `_boot+0x7a` → `PEHaltRestart+0x3e` (the `blx` at 0x801c98ec to 0x801db12b, IOPMrootDomain's halt/restart broadcast) → `messageClient` → IOMobileGraphicsFamily+0x2738 → AppleM2DisplayDrivers+0x4c40 (AppleM2CLCD `disable_backlight`) → AppleARMPlatform+0x4895/+0x6019 → AppleS5L8720X+0x17f0 → `IOCommandGate::runAction` → AppleS5L8720X+0x37cc → `commandSleep`. The S5L SWI backlight function waits for an interrupt that never comes. Patch `pe-halt-skip-pm` (0x801c98ec `blx r3` → `nop`). Then: `[mcu] iOS shut down: log synced, powering off through the MCU`, and QEMU exits.
- **QEMU** (`build/emu-r3`): `h` → `[input] key 40 down/up` and SpringBoard opens Spotlight (Home on page 1); `c` → `[mcu] shell closed: locking iOS (POWER tap)` → `[input] key 30 down/up`; `c` again → `[mcu] shell opened`. The lock press does not blank the screen in this build (SBDontLockEver; re-log §21 saw the same with SELECT), so the shell now also switches the LCD backlights off/on through MCU reg 0x22 (0x14 / 0x28, libn3ds `MCU_LCD_PWR_*`).

## 37. Volume, brightness, accelerometer (2026-11-18)
- **Volume:** Celestial's `AVSystemController` (SpringBoard imports its notifications); selectors present in the dyld cache: `sharedAVSystemController`, `setActiveCategoryVolumeTo:`, `getActiveCategoryVolume:andName:`.
- **Brightness:** SpringBoard's strings `DisplayBrightness`, `DisplayBrightnessFactor`, `…FactorWithFade`, `…Auto`; imports `IOHIDEventSystemSetProperty`. The keys occur once more in the dyld cache (IOKit's HID event system), so SpringBoard keeps its backlight state as HID event system properties; `IOHIDEventSystemClientCopyProperty` (0x337fce40) exists for clients. Kernel side: `AppleARMPWMBacklight` / `AppleARMBacklight` drive the n72 PMU through `function-backlight_*`, which our device tree does not have.
- **Accelerometer:** the kernelcache has `AppleEmbeddedAccelerometer` (LIS302DL / LIS331DLH on I2C, IOHID event service); SpringBoard has `SBAccelerometerInterface` and `com.apple.springboard.rawOrientation`. `IOHIDEventCreateAccelerometerEvent` (0x337faca4): `(alloc, timestamp r1:r2, x r3, y [sp], z [sp+4], options [sp+8])` → the common 3-axis creator with type 13.
- **QEMU** (`build/emu-r2/3`): `[mcud] volume slider 40/63 -> iOS volume 0.63`; `[mcud] iOS brightness 0.50 x factor 1.00 -> LCD backlight 500/1000` → `qom-get lcd-backlight` 79 (0x4F) and the dimmed screenshots; accelerometer `raw -16384 0 0 -> iOS 0 -1 0 g` (portrait) and `16384 0 0 -> iOS 0 1 0` after `]` `]`.
- **Accelerometer events are ignored by SpringBoard.** Its HID callback's type switch (0x51b1c: `IOHIDEventGetType` − 3, jump table; type 3 → 0x5028c keyboard, type 13 → 0x4fb44) and the accelerometer handler 0x4fb44 (reads fields 0xd0000–0xd0003; flag 0x18d3d1 = `CFPreferencesGetAppBooleanValue("SBIgnoreAccelerometerEvents")`, set at 0x50bf8; posts `com.apple.springboard.rawOrientation` at 0x4fef8, the name resolved from the literal at 0x4fed4) were never hit in 30 s of events: hardware breakpoints through QEMU's gdbstub (`-gdb tcp::12347`, `emu/gdb/rsp.py`). Same as keyboard/digitizer events in §19/§22. The handler is also reached from 0x50054, which polls accelerometer *services* with `IOHIDServiceCopyEvent(service, 13, …)`; the way in is therefore a kernel service (Apple's LIS331DLH driver on an emulated I2C device), not a client. The daemon keeps the accelerometer off unless started with `acc`.

## 38. iOS shows the 3DS clock's time (2026-11-18)
- The 3DS RTC keeps **local** time (it's set in the 3DS System Settings). The MCU driver (§33) hands it to XNU as UTC, and iOS then displayed it in its default zone, `/private/var/db/timezone/localtime` → US Pacific: RTC 00:27 Wednesday showed as 4:29 PM Tuesday.
- rootfs/recipe.json now replaces that symlink with `rootfs/files/localtime-UTC`, a 54-byte TZif v1 file generated for this project (one ttinfo: offset 0, "UTC"; no Apple data).
- iOS now shows exactly the 3DS clock: QEMU RTC 00:38 → status bar "12:38 AM", calendar "Wednesday".
- To get a true UTC epoch plus a real zone instead, set `rtc_offset=` in boot.cfg and pick a zone in Settings (docs/mcu.md).

## 39. The "Edit Home Screen" alert was iOS's one-time tip (2026-11-18)
- **Soak 4** (main `e9ddbba`, all drivers merged, 40 cycles):
  - no panic, stall, md0/log error (19,008 md0 requests), crash or jetsam;
  - again only cycle 0 showed "Edit Home Screen" after leaving the first app.
- **Cause:** the alert's strings are `REORDER_INFO_TITLE` / `REORDER_INFO_BODY` (SpringBoard.app English.lproj), and SpringBoard keeps the default **`SBDidShowReorderText`** next to them.
  - iOS shows the tip once, on the first Home out of an app on a fresh device, then sets the key.
  - QEMU soaks always start with a fresh rootfs, hence cycle 0 every time. On a real card it shows once and is remembered.
  - The §31 "Home seen as a hold" explanation was wrong.
- **Fix:** rootfs/recipe.json's com.apple.springboard.plist now sets `SBDidShowReorderText = true`.
- **Soak recovery** is now second Home, then Dismiss alone, then Dismiss + Home. Pressing Home after Dismiss on page 1 opens Spotlight, which soak 4 recorded as "stuck".
- **The same soak showed a GPU problem:** `mirror self-test: GPU operation 4 of 4 (PPF transfer) did not finish, register 0x00000000` in plain (MTTCG) QEMU. It was later reported as "self-test mismatch: 0 bytes differed". The CPU copy carried on correctly. Handed to the GPU work (agent/gpu).

## 40. Accelerometer: Apple's LIS302DL driver on an emulated sensor (2026-11-18)
- **Node:** `/arm-io/i2c0/accelerometer` (`compatible 'accelerometer,lis331dl|accelerometer,lis302dl'`, `reg 0x1d`, `interrupts 0x63` on `/arm-io/gpio`, `orientation 4`). dt_audio deleted it with the PMU. The personality `AppleLIS302DL` (provider AppleARMIICDevice, match `accelerometer,lis302dl`) lives in com.apple.driver.AppleEmbeddedAccelerometer at 0x8061a000 (`scripts/kexts.py Accel`). The kext is ARM code: disassembled with `--triple=armv6k`, and literal loads annotated with LC_SYMTAB names and kext strings.
- **Base class AppleEmbeddedI2CAccelerometer** (start at ~0x8061c5xx): an `IOTimerEventSource` (+0x8c, action 0x8061c314), a provider-less `IOInterruptEventSource` (+0x80, action 0x8061bf98) and one on the I2C nub's interrupt 0 (+0x78, action 0x8061c418). The interrupt one must exist or start fails. `/arm-io/gpio` already has `AppleS5L8900XGPIOIC` running on its fake page (`AppleS5L8900XGPIOIC::start: gpioBaseAddress …` in every boot log), so registration works; the interrupt itself never fires.
- **Polling:** `setProperties` → `ReportInterval` (0x8061b988): values 1..9999 become 10000 (µs); then the timer is armed (vtable +0x8c) or cancelled (+0xb4). The timer action reads a sample through vtable +0x3f8 (0x8061b1b8), dispatches it (+0x3c0) and re-arms with the interval minus the time spent. So no interrupt is needed while SpringBoard wants data.
- **LIS302DL methods** (vtable at 0x8061f7c4: +0x3f4 enable 0x8061b2e4, +0x3f8 read 0x8061b1b8, +0x3fc serviceInterrupts 0x8061b428). I2C helpers: read 0x8061be38, write 0x8061bed8, and a write with sub-address bit 7 (auto-increment) at 0x8061be80, all through the nub's transfer (vtable +0x35c).
  - Probe: read `WHO_AM_I` (0x0F); only the transfer result is checked.
  - Enable: `CTRL_REG1` (0x20) |= 0x40; `CTRL_REG2` (0x21) = 0x40 (BOOT), then poll 0x21 every 500 µs until bit 6 clears, `panic` after 1000 tries ("Boot bit did not return to zero"); `CTRL_REG3` (0x22) |= 0xC0.
  - Read: `OUT_X/Y/Z` 0x29/0x2B/0x2D, one byte each, signed. Value × 1187 = 16.16 fixed g (≈ 55.2 counts per g). Then the DT `orientation` flags: bit 3 swaps x/y, bits 0/1/2 negate x/y/z.
  - serviceInterrupts: `FF_WU_SRC_1` (0x31) bit 6.
- **Emulation** (`shim_mcu_iic`, tools/ios3ds-mcu/shim/mcu.c): `shim_iic_transfer` (kernel/shim/audio.c) asks it first.
  - The nub is recognised by its `reg` property (`getProperty("reg")`, vtable +0x9c; `OSData::getBytesNoCopy` 0x8018d534), cached per nub.
  - BOOT reads back cleared, `WHO_AM_I` = 0x3B, `STATUS` (0x27) = 0x0F, `FF_WU_SRC` = 0, outputs from the MCU mailbox as in docs/mcu.md §6.2. `CTRL_REG1` bit 6 switches the ARM9's sampling.
  - The shim has no divide instruction and no libgcc, so it has its own shift-subtract divide and integer square root.
- **QEMU** (`build/emu-a1`: home screen only): probe at boot, sensor powered on once SpringBoard is up, but no samples. On an iPod touch the home screen does not rotate, so nobody sets `ReportInterval`. `build/emu-a2` (Safari opened by a tap): samples start at Safari's launch; SpringBoard's `com.apple.springboard.rawOrientation` goes 0 → 1, → 4 for model pose 1, → 3 for pose 3, → 1 for pose 0. Safari's bookmarks view rotates both ways and back.

## 41. Lock turns the screen off: SBDontLockEver removed, clock-controller wait patched (2026-11-18)
- SpringBoard reads `SBDontLockEver` with `CFPreferencesGetAppBooleanValue` at 0xb42c (Thumb; CFString at 0x15848c, found by resolving `ldr/add pc` pairs against the string's address) into a global flag, next to `SBDontLockAfterCrash` and others. With it set, a lock press (`key 30`) leaves `DisplayBrightnessFactor` at 1 (§21, §36).
- **Without it, first try** (QEMU `build/emu-l1`…`l5`): `key 30` → factor 0 → LCD backlight 0; HOME → factor 1. But the screens kept showing the frame from before the lock (clock frozen), and iOS blanked again after ~10 s unless touched. That was its lock screen, which never reached the display. The fake CLCD page (`xp /4wx 0x1ffba000`) read `00000003 00000010 …` before the lock and `00000002 00000000 00000000 00000001` after the wake: AppleM2CLCD had not re-enabled the controller.
- **Cause:** the same wait as the shutdown hang (§36): AppleS5L8720X+0x37cc, in `AppleS5L8720XClockController`, is a loop `while (byte +0x38c) commandGate->commandSleep(+0x38c)` after starting a performance-state change (flags from vtable +0x39c). The clear comes from a completion interrupt the fake clock/power page never raises. Display sleep lowers the performance state, so the CLCD's power path blocked there.
- **Patch `s5l-perf-nowait`** (0x807517cc): `ldrb r3,[r5,#0x38c]; cmp r3,#0; bne` → `mov r3,#0; strb r3,[r5,#0x38c]; nop`.
- **Result** (`build/emu-l6`): CLCD `00000003 00000010` after the wake. The lock screen shows "1:32 Wednesday, November 18", the charging battery and "slide to unlock". The slide returns to the home screen, and the clock advances (1:33, 1:34). The rootfs drops `SBDontLockEver`; `SBDontLockAfterCrash` and `SBDisableAutoDim` stay.

## 42. "Where is my backlight?" flood after lock/wake (2026-11-18)
- **Soak 6** (main + accelerometer + lock, 40 cycles): PASS, but the last `[hb]` line was at t=150 s, because the console was never quiet. After the first lock/unlock, `AppleM2CLCD::safe_enable_backlight(), Where is my backlight?` appeared 3,504 times, about 3 per second.
- **Three AppleM2CLCD functions** share the format string `%s, Where is my backlight?` (VA 0x807b0b58, literal refs 0x807a9564/95c4/965c):
  - the level setter (0x807a9514);
  - `disable_backlight` (0x807a9570);
  - `safe_enable_backlight` (0x807a95d0).

  Each looks up the backlight service (`bl 0x807a9490`). When there is none, which is always on the 3DS, each only `IOLog`s (0x801a6be9) and returns. The flow is otherwise harmless; the cost was console/SD traffic. The 3DS backlight is driven by ios3ds-mcud through the MCU (§36, §41).
- **Patches** `clcd-backlight-log-set` / `-disable` / `-enable`: the `blx r3` (IOLog) at 0x807a9558, 0x807a95b0 and 0x807a9648 becomes `nop`.
- **The heartbeat stopped after ~100 s in every soak.**
  - In QEMU, 70% of console lines are empty: 1,603 of them after t=111 s in soak 7, arriving in triples, starting right after `iBoot version`. Hardware logs (run9, run13) have none, so it is a QEMU-side artefact.
  - On top of that, the daemons now print real text every few seconds.
  - Fix: the monitor counts only non-whitespace console bytes as activity, and adds a heartbeat every 30 s regardless. QEMU: heartbeats at t=10, 20, 33, 61, 76, then every 30 s to the end.

## 43. Screen-off power saving; no system sleep; the CLCD backlight retry (2026-11-18)
- **System sleep with no way back:** after a lock without `SBDontLockEver`, iOS slept the whole system after ~60 s (`System Sleep`, then AppleARMPL192VIC's `vic[0] ena 00010000`); the HOME press later reached nobody (QEMU `build/emu-p1`). The iPod wakes from the PMU interrupt, which the 3DS does not have. `IOPMAssertionCreateWithName("NoIdleSleepAssertion", 255, …)` (IOKit 0x337f0d78; the type string occurs 6 times in the dyld cache) held by ios3ds-mcud prevents it (`[mcud] NoIdleSleepAssertion held`; 94 s locked, then wake fine).
- **Where the screen-off cost was:** the ARM9 monitor loop ran ~1700 iterations/s with the screen on and ~4400/s with it off (no copies to do, so it spun faster), reading the touch ADC over SPI each time. The ARM11 still took the VBlank (GIC 42) at full rate, although nothing was injected with the CLCD off. Changes and numbers: docs/mcu.md §9.
- **ARM9 WFI:** the monitor runs with CPSR I/F masked (`cpsr=600001d3` in every log). ARM9 timer 1 (prescaler 64, 2 ms) with IE bit 9 asserts the IRQ line; CP15 c7,c0,4 returns without taking the exception; IF is cleared by hand. QEMU's arm946 halts in WFI and `arm_cpu_has_work` wakes it on the pending line. Result: the ARM9 vCPU thread goes from 39 % to 2 % host CPU.
- **`safe_enable_backlight`** (AppleM2CLCD, ARM, 0x807a95d0): `bl 0x807a9490` (find the backlight) → NULL → `IOLog("%s, Where is my backlight?", "void AppleM2CLCD::safe_enable_backlight()")` and return, leaving byte +0xea0 (enable pending) set; the success path clears it (0x807a9634). Called per frame while pending, i.e. ~3/s for the rest of the wake (soak 6). Patch `clcd-backlight-retry-off` at 0x807a963c: the three IOLog argument loads become `mov r3,#0; strb r3,[r4,#0xea0]; b epilogue` (the IOLog call at 0x807a9648 is skipped, so main's nop of that call can coexist).

## 44. Notification LED (2026-11-18)
- MCU register 0x2D takes a 100-byte pattern stream (3dbrew "MCU Services"; libn3ds `McuInfoLedPattern`, `MCU_REG_INFO_LED`): step time, smoothing, loop delay, unknown, red[32], green[32], blue[32]. The QEMU model already treated 0x2D as a stream register; it now records the pattern (`mcu-led-rgb0`, `mcu-led-steps`).
- ARM9: boot.cfg `notify_led=1` → red blink at ≤ 10 % battery without charging, off otherwise and before a power-off. QEMU: `[mcu] notification LED blinking red: battery low` (16 of 32 steps lit, 0xA00000), `[mcu] notification LED off` once the charger is back. The timing bytes are unverified, so the default is off.

## 45. Wi-Fi end to end in QEMU: an AR6014 + access point model (2026-11-18)
- **Goal:** prove the whole chain (ARM9 driver → network mailbox → `kern.ios3ds_net` → ios3ds-wifid → iOS networking) without a console. Before this, Wi-Fi had only host unit tests.
- **Model** (`emu/qemu/hw/arm/ctr_wifi.c`, docs/wifi.md §7): TMIO controller 2 at 0x10122000, an SDIO card (CCCR/FBR/CIS, function 1), the AR6014 BMI ROM and HTC/WMI firmware behaviour, and an access point with its own WPA2-PSK authenticator, bridged to `-nic user` (slirp). Power from CFG11_WIFICNT (mapped over the board's CFG11 page) and GPIO 2_2/3_12 (a small GPIO block at 0x10147000).
- **Written from the references, not from our driver:**
  - ath6kl `target.h`: function-1 addresses (`HOST_INT_STATUS` 0x400, `RX_LOOKAHEAD_VALID` 0x405, lookahead 0x408, `COUNT` 0x420, `COUNT_DEC` 0x440, window 0x474/0x478/0x47C). `htc.h`: `ENDPOINT1` = 0, so the BMI credit is `COUNT_DEC` + 4·4 = 0x450, the same register GBATEK calls COUNT4. `sdio.c`: one credit read before each BMI command, lookahead-valid wait before BMI reads, window address LSB written last.
  - dsiwifi (runs on real DSi/3DS hardware): 2-byte WMI command header, enumerated auth/crypto values (WPA2-PSK = 5, AES = 4), BSSINFO/CONNECT/DISCONNECT layouts, `00 1C` data header, `0x4000 - len` mailbox addresses, `HTC_MSG_READY` handshake.
  - IEEE 802.11i (PRF-384, EAPOL-Key, message checks), RFC 3394 key wrap, hostapd's RSN IE comparison.
- **First boot against the model:** the unchanged driver went SDIO → BMI → upload → HTC → WMI → scan → associate → 4-way → keys → link up in 1.6 s of virtual time. Both installed keys were equal to the AP's (`WMI_ADD_CIPHER_KEY pairwise ... equals the AP's TK`, `group ... equals the AP's GTK`).
- **Driver changes** (the model exposed some; the references require the others):
  - WMI and data use the endpoints from the CONNECT_SERVICE responses (status checked), not fixed 1/2.
  - The RX trailer is stripped only with `HTC_FLG_RX_TRAILER` (bit 1). Before, any flag counted.
  - Time-outs on the monitor clock: firmware start 3 s, HTC_READY and WMI_READY 5 s, scan 10 s. A missing HTC_READY used to wait forever in state `htc`.
  - A disconnect takes the link down and reconnects after 5 s. Before, it reconnected at once (a tight loop with a wrong PSK). Three failed handshakes log a psk hint.
  - **FIFO self-test:** after "SDIO up" the driver writes a pattern into the function-1 scratch registers with CMD52 and reads it back with one CMD53. If that fails, the mailbox falls back to CMD52 per byte. Reason: dsiwifi's 3DS ARM11 build reads TMIO2's data FIFO at +0x200000, not +0x10C, and nobody has tried +0x10C from the ARM9.
- **iOS side, seen in QEMU:**
  - `IOEthernetControllerGetIONetworkInterfaceObject` + "BSD Name" gave nothing (`[wifid] interface ? up`). IONetworkStack names the interface 20+ s later (`still no BSD interface with our MAC after 20 s`, then `interface is en0`). The daemon now finds it by MAC in `getifaddrs` AF_LINK.
  - **configd configures en0 by itself.** The default network set does DHCP, IPMonitor adds the default route and mDNSResponder gets the DHCP DNS server. In the first run the daemon never ran `ipconfig` (no name yet), yet en0 had 10.0.2.15. `ipconfig set` is now only a fallback after 15 s without an address.
  - `ios3ds-nettest` (new; `nettest=` in wifi.cfg, passed through the mailbox, protocol version 2):
    ```
    [nettest] en0 address 10.0.2.15 netmask 255.255.255.0 (after 11 s)
    [nettest] default route via 10.0.2.2 (interface index 2)
    [nettest] dns example.com -> 172.66.147.243
    [nettest] http 10.0.2.2:8080/ios3ds-nettest -> HTTP/1.0 200 OK | ios3ds-wifi-test-aa4ee772 path=/ios3ds-nettest (185 bytes, 0 s)
    [nettest] all steps passed
    ```
    The host's HTTP server logged `GET /ios3ds-nettest`.
- **Status-bar Wi-Fi icon: not reachable.** SpringBoard's Wi-Fi state comes from configd's `WiFiManager.bundle`. Its strings show it only drives an `IO80211Interface`/`IO80211Controller` through Apple80211 ("Could not obtain IO80211Interface bsd name"), so an IOUserEthernet `en0` stays invisible to it.
- **Unverified on hardware** (docs/wifi.md §6 lists the log line that settles each):
  - the ARM9 powering the chip and driving TMIO2;
  - the ARM9 FIFO at 0x1012210C;
  - the stub setting `hi_board_data_initialized`;
  - Type4 firmware answering HTC/WMI as modelled;
  - BSSINFO from an all-channel scan with BSS filter 1;
  - capability bits 0 in the firmware's association RSN IE;
  - dsiwifi's post-REG_DOMAIN host-interest write, which we skip.

## 46. CoreAnimation's compositor on the PICA200 (2026-11-18)
Design and RE notes: docs/gpu.md §7.
- **RE (QuartzCore in the user's dyld cache, 8C148):** helper scripts resolved `__picsymbolstub4` stubs through the indirect symbol table and disassembled all of `__text` (ARM). `M2Display::M2Display` (0x346de5b8) probes `eagl_context(2)`, then `(1)`, storing the API at `+0x17c` and the surface format at `+0x180` from a literal pool: `'BGRA'` (0x346de6a4) for API 2, `'L565'` (0x346de6a8) for API 1. `GLESContext::make_buffer_current` (0x3470d274) binds the display IOSurface as a 0x84F5 texture via `attachImage:toCoreSurface:invertedRender:NO`, NEAREST/CLAMP, and attaches it to an FBO. `glGetString` is called with `GL_EXTENSIONS` and 0x8A1E (0x3470dd18/dd24).
- **Runs (QEMU, plain):** (1) `glBindTexture(0x84F5)` and `glGetIntegerv(0x84F8)` errors, `CoreAnimation: rendering error 500`, black screen with one icon; (2) after accepting 0x84F5: GL error in `GetString` (traced with the new `IOS3DS_GL_TRACE`: `engine+0x49bc` → `_api_GetString`); (3) no errors, still black but one icon; (4) cause: `glFlush` was a no-op and `glFinish` synced only the bound framebuffer while CA flushes with FBO 0 bound — the GPU frames never reached the IOSurface; the one icon came from a draw that fell back to the CPU rasterizer. With `flush_all` the home screen appears.
- **Pixels:** home screen GPU compositor vs software compositor, same moment: 2181 of 192000 screenshot pixels differ, by at most 1. Spotlight, keyboard, page swipes render the same way (later frames differ only where the two runs are at different animation times or the status-bar clock differs). `--icount 2`: same.
- **Gate:** `[gl] CA scene check: GPU vs CPU rasterizer: 6 of 9216 pixels differ by more than 24 (max 102)`, `[gl] CA compositor: OpenGL ES on the PICA200 (GPU self-check and scene check passed)`; with boot.cfg `ca_gpu=0`: `[gl] CA compositor: software (boot.cfg ca_gpu=0 or gpu=0)` and the software compositor runs as before.
- **Cost (QEMU):** per CA frame on the GPU path, render thread CPU 9.7–15.2 ms (plain), 19–27 ms (`--icount 2`), with one read-back of the whole display surface (~616 KiB) per frame. SpringBoard CPU in 60 s sampler windows during page swipes: GPU 4.97 / 7.05 s, software 4.16 / 5.40 s, i.e. ~25 % more in QEMU. QEMU runs CA's software compositor at host speed and models GPU operations with fixed delays, so this says little about hardware; the per-frame read-back (PPF + copy-out + per-pixel byte rotation) is the known overhead and the next thing to remove (docs/gpu.md §7). The `[gl] CA:` lines measure it on hardware.
- **App launch / exit:** my own scripted taps right after the home screen came up opened nothing in *either* compositor (identical behaviour, not a rendering issue). `emu/soak.py --cycles 6` on the GPU compositor (merged main): **PASS**, apps open (Game Center, Photos "Updating Library", Contacts, …; app-vs-home difference 39–99) and Home returns to an identical home screen (difference 0.0) every cycle; no panic, crash, stall or mirror failure; `[gl] CA: 600 frames; per frame … render thread CPU 8933 us, 1.0 command lists, 1.0 read-backs (604 KiB)`.

## 47. Mirror self-test "mismatch" that was a timeout (soak 4) (2026-11-18)
- **Report (soak 4, plain MTTCG, main e9ddbba):** `[gpu] mirror self-test: GPU operation 4 of 4 (PPF transfer) did not finish, register 0x00000000`, then `self-test FAILED (0 bytes differ)` and `FAILED (self-test mismatch)`. The failure dump right after shows upload, texture and target exactly as expected: the read-out was late, not wrong. "4 of 4": the self-test mirrors one framebuffer set (upload, texture copy, draw, one read-out); live frames have 5 operations with two sets.
- **Why late:** the QEMU GX model completes an operation from a virtual-time timer that needs the BQL; the shim's self-test polled GX registers in a tight loop (each MMIO read takes the BQL). On a busy host (several QEMUs) the timer can wait a long time. Measured with the merged (rel) QEMU on a loaded host: upload 44871 ticks, read-out 75494 ticks (~45/75 ms) for operations modelled at 0.4 ms — the 200 ms limit had a margin of ~3. Not reproduced as a failure in this worktree.
- **Fixes:** 1 s per operation; thread-context waits poll in ~1 ms slices with a 10 µs `IODelay` between; the QEMU model runs due operations when a GX register is read; `pm_selftest` returns `PM_SELFTEST_TIMEOUT` with the operation and register, logged as `self-test aborted, GPU operation N did not finish` and reported as `GM_WHY_TIMEOUT` (ARM9: `display mirror FAILED (GPU timeout: operation N of a frame did not finish, register …)`). Host test: a hung GPU gives `PM_SELFTEST_TIMEOUT`, operation 3.
- **After the fix (plain QEMU, this worktree, merged main):** self-test read-out ~55 ms → passes with margin; mirror active in plain and `--icount 2` boots.

## 48. Timer interrupts while the screen is off (2026-11-18)
- **Shim counters** (`xp /24wx 0x20840040`: +0x4C timer IRQs, +0x58 deferred FIQ retries, +0x78 VBlanks, +0x80 clock reads, +0x8C `set_decrementer` calls, +0x98 IRQs): with the screen on, 87 timer IRQs/s, 296 decrementer sets/s, 1363 clock reads/s. With it off, 140, 415 and 2055. No deferred retries in either.
- **Sampling** (`set_decrementer` = shim+0x18, hardware breakpoint through QEMU's gdbstub, frame-pointer backtraces 18 deep):
  - User frames end in `usleep` (dyld cache 0x32c9f117), called from ios3ds-input 0x70bc and ios3ds-mcud 0x7008 (both ARM `bl`, read with `llvm-objdump --macho`), or in `sleep` (0x32ce31ef, ios3ds-wifid).
  - Kernel-only chains go through `thread_call_allocate+0x2bc` (0x80025019, the thread_call worker's block), through `assert_wait_deadline+0x212`, and through `STRDUP+0x764` (0x8005c52d).
  - 0x8005c4f0 is XNU's `serial_keyboard_poll`: a loop calling the UART rx hook (0x8006b004 with 0,1,0,1), then `clock_interval_to_deadline(16, …)`, `assert_wait_deadline`, `thread_block`, i.e. 62.5 wakeups/s forever, with nothing to read (our rx-ready patch always says no). The boot-arg `serial=3` (bit 1 = keyboard) started it; `serial=1` keeps the serial console without it. After the change the chain is gone from the samples, and console output is unchanged.
  - The thread_call worker's function call (`blx r8` at 0x80024fa6) is mostly BSD networking callouts: `ip_output+0x86f4`, `net_del_domain+0x5c` and `+0xb8`, `in6addr_local+…`, `sodisconnect+0xb9a`, `netboot_root+0x4` (nearest exported symbols). The mix is the same with the screen on and off; only their share of all timer arms doubles when off.
- **No CoreAnimation:** no SpringBoard or QuartzCore user frames among the timer arms, so the guess in §43 was wrong.
- **Changes:**
  - `serial=1`;
  - ios3ds-input at 10 Hz while `screen_off` (read from the MCU mailbox; samples queue in the ring, and hold timing now counts real sleep);
  - ios3ds-mcud at 4 Hz while off.
- **Result** (QEMU): off 146 IRQs/s (143 timer) instead of 245; on 127 instead of 144; ARM11 vCPU thread 10 % instead of 14 % host CPU while off. Table in docs/mcu.md §9.

## 49. Microphone: iOS records the 3DS mic (2026-11-18)
- **No input at first:** AudioSession reported input available 0, route "", and `AudioSessionSetActive(PlayAndRecord)`/`AudioQueueNewInput` failed with `'nopi'`. An iPod2,1 records only from a wired headset.
- **Codec side:** AppleCS42L58Audio's input data sources come from `audio0` properties (0x80660294): `line` always, `imic` with `ain2b-intmic`, `emic` with `ain2a-extmic`. AppleEmbeddedAudio names them (0x80655a04). Adding `ain2b-intmic` alone changed nothing.
- **Routing:** the decision is made by mediaserverd's HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` (found through the HFS catalog; stripped).
  - It picks `RoutingSettings_<product>` by matching `hw.model` against a 4-byte table at file offset 0x287210 (`K48 K93 K94 K95 K66 N18 N72 N81 N82 N88 N90 N92 N94`).
  - N72 has no built-in mic port.
  - Patch (`rootfs/recipe.json`): the N81 slot (iPod touch 4G) reads `N72`, the N72 slot `X72`. Result: input available 1, route `SpeakerAndMicrophone`, speaker output and amp volume unchanged.
- **Data path:**
  - The 3DS MIC block (0x10162000, GBATEK's DSi layout) runs at I2S2 / 4 = 11,901 Hz.
  - First version: drained by the ARM9 monitor loop. In QEMU it overran the 16-word FIFO constantly. It now runs in the shim (ARM11), which services every 1 ms while the mic is on; the 3DS's own mic service also runs on the ARM11.
  - The ARM9 powers the CODEC ADC while the shim says iOS records. `boot.cfg mic=0` keeps everything off.
  - Safety: no data for 250 ms, a FIFO that never empties, or the block switching itself off each lead to one log line, mic off, and silence.
- **Findings on the way (QEMU):**
  - iOS's input chain high-passes the input and adds a fixed +23.65 dB (×15.23; an 8000 sine clipped). The shim takes it off again (`AUD_MIC_GAIN_Q15`).
  - The 1 ms service deadline also has to hold while the RX channel is idle between two queued commands.
  - The mic must stay on across such gaps (1 s hold); switching it off there punched 50 ms holes.
  - With main's older md0 code (interrupts off while waiting), services stalled and samples were lost. Fixed by rebasing onto the md0-with-interrupts change.
  - Some recordings were corrupted, always bit-identically, by isolated spikes while the shim's own output was clean. Cause: the L/R word phase was carried over from the previous input stream; after an odd-length stream every frame straddled two. The phase now comes from the buffer address.
  - A 10 ms write-ahead of the input ring, tried first, made no difference and was removed.
- **Result (`IOS3DS_MIC=sine:440:1000`, real FIFO, icount 2):** five of five recordings in RecordAudio (run 16) and five of five in PlayAndRecord with the speaker output running (run 17) fit 440.03 Hz at amplitude 1003.0, residual 41 dB. 0 overruns, 0 underruns, 0 resyncs.
- **Bit-exactness:** iOS's own filtering makes app-level bit-exactness impossible. The host test shows it below iOS: a counter through the shim's FIFO drain and RX emulation arrives as an exact scaled, interpolated ramp, L = R in every frame, including after an odd-length stream.
