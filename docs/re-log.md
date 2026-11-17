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
