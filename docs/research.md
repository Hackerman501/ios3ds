# ios3ds research notes (M0, updated during M1/M2)

Every claim here carries its source. How sure I am of each one:
- **[verified-src]**: read in primary source code or an ARM TRM, with link.
- **[verified-bin]**: confirmed by analysing the real 4.2.1 binaries with the
  tools in this repo. Reproduction steps are in [re-log.md](re-log.md).
- **[3dbrew-snippet]**: 3dbrew.org refuses automated fetches (HTTP 403), so
  only search-engine excerpts were seen. Treat these as unverified.
- **[unverified]**: an assumption. Each one has a hardware test that will
  settle it (listed in §6).

Pinned source revisions:
- `L` = [Luma3DS@1441535d](https://github.com/LumaTeam/Luma3DS/tree/1441535d222002353b79d290b6200ceab2e32cb0)
- `B` = [boot9strap@934e1009](https://github.com/SciresM/boot9strap/tree/934e10092a9caab0dbc83038449878862b7fc7f7)
- `N` = [libn3ds@a3975be4](https://github.com/profi200/libn3ds/tree/a3975be4d544a0a60406b9570e504092b43f1ea2)
- `FLL` = [linux-3ds/firm_linux_loader@ecb0f0d5](https://github.com/linux-3ds/firm_linux_loader/tree/ecb0f0d5a8d8aaafaf5edca279098b52ecfa71a0)
- `LX` = [linux-3ds/linux@7071ec29](https://github.com/linux-3ds/linux/tree/7071ec29fd3ebd55d00a70cb6482e98d4702c5f2)
- `GM9` = [GodMode9@b1e0a97e](https://github.com/d0k3/GodMode9/tree/b1e0a97e99f4ada0e2aab27069c5b521c2d10146)

ARM documentation:
- `[1176]` ARM1176JZF-S TRM r0p7, [DDI 0301H](https://documentation-service.arm.com/static/5e8e294efd977155116a6ca3)
- `[MPC]` ARM11 MPCore TRM r2p0, [DDI 0360F](https://documentation-service.arm.com/static/5e8e1e0388295d1e18d368b2)
- `[ERR]` ARM11 MPCore Errata Notice v25, [link](https://documentation-service.arm.com/static/5ed631c6ca06a95ce53f924f)

---

## 0. Checking the claims in the task brief

| Claim | Verdict | Source |
|---|---|---|
| O3DS ARM11 MPCore, ARMv6K, 2 cores, 268 MHz | **Correct.** 268,111,856 Hz exactly. The MPCore private timer runs at half that (134,055,928 Hz). | N/include/arm11/drivers/timer.h; LX/arch/arm/boot/dts/nintendo3ds_ctr.dts (two `arm,arm11mpcore` CPUs); [MPC] p.1-2 "implements ARMv6K" [verified-src] |
| ARM9 = ARM946E-S | **Correct**, ~134 MHz | N/source/arm9/start.s `.cpu arm946e-s` [verified-src]; clock rate [3dbrew-snippet] |
| 128 MB FCRAM | **Correct**: 0x20000000–0x27FFFFFF | N/include/mem_map.h; LX DTS `memory@20000000` [verified-src] |
| 6 MB VRAM | **Correct**: 0x18000000, two 3 MB banks | N/include/mem_map.h [verified-src] |
| PICA200 GPU | Plausible, not checked in source | 3dbrew Hardware [3dbrew-snippet] |
| Top 400×240, bottom 320×240, resistive single-touch | Resolutions **correct**. Touch is a 12-bit TSC over SPI, one contact. "Resistive" was not confirmed in source. | L/arm9/source/screen.h; LX nintendo3ds.dtsi `tsc@0` [verified-src] |
| iPod touch 2G: ARM1176JZF-S @ 533 MHz, 128 MB, MBX Lite, 320×480 | ARM1176 core **confirmed** (the device tree says `ARM,1176jzf-s`). MBX **confirmed** (`AppleMBX` kext, `/arm-io/mbx`, `MBXGLEngine.bundle`). Clock, RAM and screen not yet checked against a primary source. | [verified-bin] devicetree.txt, kernelcache.txt |
| "iPod touch 2G has no baseband" | Consistent: no baseband kexts or device-tree nodes | [verified-bin] |
| Apple never published ARM XNU for iOS 4 | Consistent: the kernel is `xnu-1504.58.28`, and only the Mac-only xnu-1504.15.3 (10.6.8) is open source | [verified-bin] version string |
| Wi-Fi chip | Was not in the brief, but note it is **Broadcom** (`AppleBCMWLAN`), not Marvell | [verified-bin] |
| winocm's ~2013 ARMv6/v7 XNU port | **Exists** as [darwin-on-arm/xnu](https://github.com/darwin-on-arm/xnu) (winocm/xnu redirects there). It even has `pe_s5l8720x.c`, but **no ARM11 MPCore support**, and it uses MCRR range cache ops that MPCore lacks. | §3.9 |
| HackiOS / ios-sweet, devos50 + samhenrigold qemu-ios, Light Touch | **All exist** (§5). ios-sweet and HackiOS use *open-source arm64 XNU*, not Apple's kernelcache. samhenrigold's qemu-ios (inside Light Touch) boots *stock* 4.2.1 n72ap to the home screen and forwards GL to the host. devos50's emulates iOS 2.1.1 with **software** rendering, not GL forwarding. | §5 |

---

## 1. 3DS: memory map and boot handoff

### 1.1 Physical memory (N/include/mem_map.h, LX DTS) [verified-src]

| Range | What | Who sees it |
|---|---|---|
| 0x00000000 (+0x10000, 0xFFFF0000 mirror) | Boot11 ROM, 64 KiB | ARM11 |
| 0x08000000–0x080FFFFF | ARM9 RAM, 1 MiB | ARM9 |
| 0x10000000–0x100FFFFF | ARM9 AHB IO: CFG9 @0x10000000, IRQ9, TMIO1 (SD) @0x10006000, TMIO3 @0x10007000, PXI9 @0x10008000, AES | ARM9 |
| 0x10100000–0x101FFFFF | Shared IO: CFG11 @0x10140000, PDN @0x10141000, I2C2 @0x10144000 (MCU), HID @0x10146000, GPIO, SPI, PXI11 @0x10163000, TMIO controller 3 when mapped to ARM11 @0x10100000 | both |
| 0x10200000+ | ARM11 AXI IO: CDMA, LCD @0x10202000, GPU/PSC/PDC @0x10400000 | ARM11 |
| 0x17E00000 (8 KiB) | MPCore private: SCU +0x0, GIC CPU if +0x100, private timer +0x600, watchdog +0x620, GIC distributor +0x1000 | ARM11 |
| 0x18000000–0x185FFFFF | VRAM, 6 MiB | both |
| 0x1FF00000 | DSP RAM, 512 KiB | both |
| 0x1FF80000–0x1FFFFFFF | AXI WRAM, 512 KiB. The **top 1 KiB is reserved**: core-0 spin stub @0x1FFFFC00, boot11 RAM vectors @0x1FFFFFA0, core-1 entry @0x1FFFFFDC, core-0 entry @0x1FFFFFFC. | both |
| 0x20000000–0x27FFFFFF | FCRAM, 128 MiB | both |
| 0xFFF00000 | ARM9 DTCM, 16 KiB | ARM9 |
| 0x17E10000 | L2C-310 (**New 3DS only**; the O3DS has **no L2**) | — |

ARM11 L1 is 16 KiB I-cache plus 16 KiB D-cache (N/source/arm11/drivers/cache.s) [verified-src].
The 3DS MIDR is not documented anywhere I could read [unverified], so M1 prints it.

### 1.2 FIRM format and Luma chainloading [verified-src]

**Header** (B/stage2/arm9/source/firm.h; [TuxSH/firmtool](https://github.com/TuxSH/firmtool)):

| Offset | Field |
|---|---|
| 0x000 | `FIRM` |
| 0x004 | priority |
| 0x008 | ARM11 entry |
| 0x00C | ARM9 entry |
| 0x010 | reserved; **byte 0x10 bit0 = screen-init request**, bit1 = skip bootrom lockout |
| 0x040 | 4 section headers `{offset, load addr, size, copy method, sha256}` |
| 0x100 | RSA signature |

Our `scripts/mkfirm.py` writes this format.

**Luma `checkFirm`** (L/arm9/source/firm.c):
- Sections must be 512-aligned and have correct SHA-256 hashes. The signature is not checked.
- Each entry point must lie inside a section.
- A zero ARM11 entry is allowed.
- Sections may only target 0x08000000–0x08100000, 0x18000000–0x18600000, 0x1FF00000–0x1FFFFC00 or 0x20000000–0x28000000.
- Sections must not overlap the file buffer at 0x20001000.

**Launching a payload:** put it in `sd:/luma/payloads/`. Either hold a key at
boot (e.g. `x_ios3ds.firm` with X), or hold START for the payload menu
(L/arm9/source/fs.c, main.c).

### 1.3 CPU state at payload entry [verified-src]

**ARM9** (L/arm9/source/start.s, chainloader.c):
- SVC mode, IRQ and FIQ masked. MPU and caches off; TCMs on.
- `r0=argc`, `r1=argv`. `argv[0]` is the payload path.
- If screen-init was requested, `argv[1]` points to the framebuffer pointers
  `{0x18300000, 0x18300000, 0x18346500}` / `{0x18400000, …}`.
- argv lives on an ITCM stack, so copy it before touching ITCM.

**ARM11 core 0** (L/arm11/source/main.c, start.s):
- Spins in a stub at 0x1FFFFC00 until the ARM9 writes the entry address to
  **0x1FFFFFFC**. Luma writes it after copying the sections.
- At entry: SVC mode, A/I/F masked, SCTLR = reset value 0x54078.
  **MMU, D-cache and I-cache are off and vectors are low.** Caches are invalidated.

**ARM11 core 1**:
- Still in boot11, waiting for an IPI. Its entry goes at **0x1FFFFFDC**,
  followed by SGI 1 (N/source/arm11/drivers/pdn.c; GM9 sys.c).
- **Leaving it parked costs nothing**, which is what M1 needs.

**Screens:**
- With the screen-init flag, Luma brings the LCDs up in BGR8 with stride 0x2D0.
- Without it the screens are usually unpowered, so you get a **black screen**.
- M1 sets the flag *and* re-points PDC0 to its own buffer.
- The full self-init sequence (PDN_GPU_CNT, LCD regs, MCU I2C 0x25 reg 0x22) is
  in L/arm11/source/main.c `initScreens` and N/source/arm11/drivers/gfx.c.

**Framebuffer format** (N/include/drivers/gfx.h; PDC0 @0x10400400):
- Format codes: 0 ABGR8, 1 BGR8, 2 BGR565, 3 A1BGR5, 4 ABGR4.
- Memory is stored **as 240-pixel columns**: pixel (x,y) on the top screen is at
  `(x*240 + 239-y)*bpp`.
- Holding the 3DS sideways, like a book, makes the top screen a 240×400 portrait
  display in natural row order. That is close to the iPod's 320×480, and it is
  how XNU's linear-framebuffer console can draw without rotation (see §2.5).

### 1.4 SD card access from the ARM11 [verified-src]

- `REG_CFG9_SDMMCCTL` (0x10000020) is ARM9-only:
  - bit8 maps TMIO controller 3 to the **ARM11 at 0x10100000**
  - bit9 routes the SD slot to TMIO1 (ARM9) instead of TMIO3
  - Source: N/include/arm9/drivers/cfg9.h, tmio_config.h.
- **So the ARM9 can hand the SD slot to the ARM11 by writing bit8=1, bit9=0.**
  The ARM11 then drives it directly: FIFO at 0x10300000, GIC IRQ 66.
  libn3ds has an ARM11-side test for this (N/tests/sd_card.c).
- linux-3ds does *not* do this. It keeps SD on the ARM9 and serves it as
  virtio-blk over PXI ([arm9linuxfw](https://github.com/linux-3ds/arm9linuxfw)).
- **Plan:**
  - M1 does SD I/O on the ARM9 (FatFs) because it's simplest and proven.
  - For M3 we can choose between an ARM11 TMIO driver inside XNU (fewer moving
    parts at runtime) and a PXI block server.

### 1.5 Interrupts and timers on the ARM11 (N/include/arm11/drivers/interrupt.h) [verified-src]

The interrupt controller is a GIC-style MPCore distributor. IDs:
- private timer 29, watchdog 30
- PDC0/1 VBlank 42/43
- PXI 80/82/83
- I2C 84/85/92
- TMIO3 66
- HID_PADCNT 91 (normal button reading is polling 0x10146000)
- GPIO 96–115

This replaces the S5L8720's two PL192 VICs (`/arm-io/vic compatible = vic,pl192`, `vic-stride 0x1000`) [verified-bin].

---

## 2. What the iOS 4.2.1 kernel expects from iBoot

Kernel identity [verified-bin]: `xnu-1504.58.28~3/RELEASE_ARM_S5L8720X`,
Mach-O ARM v6, `__TEXT` at **0x80001000**, entry **0x80067050**, 99 prelinked
kexts. Details and commands are in [re-log.md](re-log.md) §3–4.

### 2.1 Register and MMU state at entry [verified-bin, entry stub disassembly]

- ARM state, MMU off. The stub itself masks IRQ/FIQ.
- **r0 = physical address of `boot_args`.** No other register is read
  (r1 is cleared).
- Vectors: the kernel builds its own L2 table and maps the vector page at
  **0xFFFF0000** with SCTLR.V=1. No VBAR, no SMC.
- SCTLR bits it sets: `M C W Z I V U XP` (0x00C0380D), with XP=1 (extended page tables).

### 2.2 boot_args layout

These fields are read by the entry stub [verified-bin]:

The layout matches winocm's reimplementation
([darwin-on-arm pexpert/pexpert/arm/boot.h](https://github.com/darwin-on-arm/xnu/blob/master/pexpert/pexpert/arm/boot.h))
and [samhenrigold/qemu-ios `ios-boot-args.h`](https://github.com/samhenrigold/qemu-ios/blob/main/include/hw/arm/ios-boot-args.h),
which boots this exact build.

```c
struct Boot_Video { uint32_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };
/* v_display: 1 = GRAPHICS_MODE, 2 = FB_TEXT_MODE; v_depth: bpp | rotate<<8 | (scale-1)<<16 */
struct boot_args {
    uint16_t Revision;          /* 0x00  1  (not read by the entry stub) */
    uint16_t Version;           /* 0x02  MUST be 5 on 4.2.1: pe_identify_machine panics "Epoch Mismatch" otherwise [bin, re-log 11] */
    uint32_t virtBase;          /* 0x04  0x80000000                                   [bin] */
    uint32_t physBase;          /* 0x08  16 MB aligned (supersections)                [bin] */
    uint32_t memSize;           /* 0x0C  MUST be a multiple of 1 MB: the map loop is
                                         `subs r10,#0x100000; bne` and never ends otherwise [bin] */
    uint32_t topOfKernelData;   /* 0x10  PHYSICAL; becomes TTBR; 16 KB aligned; kernel zeroes 0x9000 [bin] */
    struct Boot_Video Video;    /* 0x14  copied field by field by the routine at 0x801dc720 [bin, via research agent] */
    uint32_t machineType;       /* 0x2C */
    uint32_t deviceTreeP;       /* 0x30  KERNEL VIRTUAL address (passed to DTInit)     [bin, via research agent] */
    uint32_t deviceTreeLength;  /* 0x34 */
    char     CommandLine[256];  /* 0x38  -> sizeof == 0x138 */
};
```

On the real device, iBoot places the kernel at PA 0x08001000, the device tree
after it, and boot_args at PA 0x08825000. DRAM on the S5L8720 is at
0x08000000 ([samhenrigold from-ipsw.md](https://github.com/samhenrigold/qemu-ios/blob/main/docs/ipod/from-ipsw.md);
[devos50 ipod_touch_2g.h](https://github.com/devos50/qemu-ios/blob/ipod_touch_2g/include/hw/arm/ipod_touch_2g.h)).
GenericBooter uses the same order: kernel → (ramdisk) → device tree →
boot_args → `topOfKernelData` rounded up to 1 MB
([bootx.c](https://github.com/darwin-on-arm/GenericBooter-next/blob/master/init/bootx.c)).
Whether `v_baseAddr` is physical or virtual is [unverified]. winocm's ports
store the physical address, and M2 will test that first.

### 2.3 Load addresses on the 3DS (derived) [verified-bin + derived]

- `physBase = 0x20000000` (FCRAM start; 16 MB aligned).
- `virtBase = 0x80000000`.
- Kernel image at PA `physBase + (vmaddr - virtBase)`, i.e. `__TEXT` at
  **0x20001000**. Each segment goes at `0x20000000 + (vmaddr - 0x80000000)`,
  and the kernelcache spans 0x20001000–0x2081B000.
- **Collision:** Luma reads the .firm to **0x20001000** before copying sections
  (§1.2). So the kernel can't be a FIRM section there, and the loader has to
  read it from SD *after* the FIRM is unpacked. The M1 design does exactly that.
- Device tree and boot_args go after the kernel. `topOfKernelData` is the next
  16 KB boundary after all of that, and memSize stays ≤ 128 MB.
- The iOS kernel treats every byte below `topOfKernelData` as used. Our own
  loader stub, log ring buffer and framebuffers live in **VRAM / AXI WRAM**, so
  all of FCRAM goes to XNU.

### 2.4 Device tree [verified-bin]

**Format:** Apple flattened tree.
- A node is `u32 nProps, u32 nChildren`, then its properties, then its children.
- A property is `char name[32]`, `u32 len`, then the value padded to 4
  ([darwin-on-arm device_tree.h](https://github.com/darwin-on-arm/xnu/blob/master/pexpert/pexpert/device_tree.h)).
- Later iOS versions use len bit31 as a "placeholder" flag. **No property in the
  8C148 tree has it set** [verified-bin].
- Parser and serialiser: `scripts/fwlib/appledt.py`. It round-trips the n72ap
  tree byte for byte. Its VERS tag is `EmbeddedDeviceTrees-617.53.1`.

**The template in the IPSW** (38,940 bytes, 77 nodes) has **zero-filled slots
that iBoot fills in**:
- `/chosen/memory-map` has 16 × `MemoryMapReserved-N` (8 bytes each). iBoot
  *renames* them to `Kernel-*`, `DeviceTree`, `BootArgs`, `RAMDisk`, and so on.
- `/memory reg`, the `/cpus/cpu0` frequencies, serial numbers, and
  `/chosen/debug-enabled`.

The kernel looks these names up by string: `Kernel-__HEADER`/`__SYMTAB`/`__KLD`/
`__LINKEDIT`, `DeviceTree`, `RAMDisk` (memory-map entries), `root-matching`,
`SecureRootName`, `diag-*`.

**What ios3ds will do:** generate its own tree from the template.
- Keep the generic nodes.
- **Delete every S5L8720 hardware node** so the matching Apple kexts never load.
- Fill `/memory`, `/chosen/memory-map` (Kernel-*, DeviceTree, optional RAMDisk)
  and the frequencies (CPU 268,111,856 Hz; timebase 134,055,928 Hz if we use the
  MPCore private timer).
- Add nodes for our own drivers.

**`/arm-io`:**
- `device_type = s5l8720x-io`. This string selects the kernel's built-in SoC
  support (§2.6).
- `ranges` maps child 0x0 → PA 0x38000000 and 0x10000000 → 0x18000000.
- **The second range lands on 3DS VRAM.** That's a harmless coincidence once
  those nodes are deleted, but a good reason to rewrite `ranges` to the 3DS IO map.

### 2.5 Console path (for M2) [verified-bin + unverified]

- `osfmk/console/video_console.c` is compiled in.
- The 32 bpp CLUT descriptor `BBBBBBBBGGGGGGGGRRRRRRRR` and `boot-console` /
  `-progress` / `BootCLUT` strings are present.
- XNU's standard behaviour (open-source xnu-1504 `pexpert/gen/pe_gen.c`) is a
  text console on the boot_args framebuffer when `Video.v_display` selects text
  mode [unverified for this ARM build].
- **Plan:** M2 points Boot_Video at a 32 bpp 240×400 buffer, which the 3DS can
  scan out as format 0 (ABGR8). We then check whether kernel printf reaches it.
- **Fallback that needs no kernel cooperation:** patch `_kprintf` (0x801dcccc)
  and the serial `putc` to append to a RAM ring buffer. The ARM9 dumps that
  buffer to `sd:/ios3ds/log.txt`. A pointer dump on a hung kernel still recovers
  the log.
- **`-v` does not occur anywhere in the decompressed kernelcache.** I searched
  for `-v\0` in every segment and found nothing [verified-bin]. One research pass
  claimed it was present; I couldn't reproduce that. So verbose text has to come
  from `Video.v_display = FB_TEXT_MODE`, or from our own console hook.
- `debug`, `serial`, `io`, `rd`, `rootdev`, `msgbuf`, `maxmem`, `cpus`, `wdt`
  and `kdp_match_name` are present.
- AMFI has `amfi_allow_any_signature`, `amfi_get_out_of_my_way`,
  `cs_enforcement_disable` and `amfi_unrestrict_task_for_pid`. `pio-error` is
  absent.
- `PE_i_can_has_debugger` (exported, 0x801dc5d0) returns a global (0x802819f8).
  Platform init fills that global from **`/chosen/debug-enabled`**, and **we
  write the device tree** [verified-bin by the research agent].
  - samhenrigold boots stock 8C148 with
    `amfi_allow_any_signature=1 cs_enforcement_disable=1 serial=3 debug=0x8`
    and reports that the AMFI flags take effect.
  - So code signing may not need a kernel patch at all. M3 will confirm this.

### 2.6 The SoC layer inside the kernel [verified-bin]

`pe_identify_machine` dispatches on `/arm-io device_type` through a built-in table:
- `s5l8900x-io`, `s5l8720x-io`, `s5l8722x-io`
- `s3c2410-io`, `integratorcp-io`, `olocreek-io`, `omap3430sdp-io`, `s5i3000-io`

Each entry carries clock and timebase constants plus SoC hooks: timer, serial,
and `cpu-debug-interface`.

Built-in UART drivers: `uart,16550`, `uart,s5i3000`, `uart,s5l8{900,720,920,930,940,950}x`.

So the timer and early serial live **in the kernel proper**, mostly in Thumb
code. Everything else is IOKit kexts matched via the device tree.

---

## 3. ARM1176JZF-S vs ARM11 MPCore, the parts XNU touches

The kernel scan is in `scripts/kscan.py`, and its output is in re-log.md §5.

### 3.1 Identification
- MIDR: ARM1176 part 0xB76 (e.g. 0x410FB767); MPCore part **0xB02** (r2p0 = 0x410FB024) ([1176] Table 3-4; [MPC] Table 3-2).
- The kernel's MIDR reader (0x80060d58) stores MIDR, checks arch==0xF, and reads ID_ISAR1. That value is identical on both cores (0x12002111). No part-number compare was found at the read site [verified-bin]; downstream uses get checked in M2.
- The cache geometry is derived from CTR (0x80060f8c) [verified-bin]. The exception is the entry stub's clean loop, which hard-codes 16 KB 4-way 32 B. **That matches the 3DS 16 KB D-cache.**
- FPSID: 0x410120B3 vs 0x410120B4. Both VFPv2, enabled the same way via CPACR and FPEXC.

### 3.2 Caches
- MPCore **D-cache is PIPT** ([MPC] p.7-3), so there is no D-side page colouring. The I-cache is VIPT and doesn't alias at 16 KB.
- MPCore lacks the MCRR range ops (ID_MMFR2[11:8]=0), c7,c10,6 (dirty status) and c7,c13,1. **The iOS kernel uses none of them** [verified-bin].
- Write-through Normal memory is treated as uncached on MPCore. That costs performance only.

### 3.3 TrustZone
MPCore has no Security Extensions. SCR/SDER/NSACR/VBAR/MVBAR/ISR/SMC are all
absent, and "c12 – not used" ([MPC] Table 3-2). **The kernel uses no SMC and no
c12 access** [verified-bin], so nothing needs to change.

### 3.4 Registers that need patching

| Encoding | Kernel sites | On MPCore | Mitigation |
|---|---|---|---|
| `c15,c2,4` Peripheral Port Memory Remap | MRC 0x80060274, MCR 0x80064208 | **absent** (UNPREDICTABLE, [MPC] §3.1) | NOP; map IO as Device + XN |
| `c6,c0,2` IFAR | MRC 0x800662e0, 0x8006632c (prefetch abort) | **absent**: MPCore updates only IFSR ([MPC] Table 5-12) | use `LR_abt - 4` |
| `c1,c0,1` ACTLR | setter at 0x80067bf0 | **same register, different bits**: 1176 bit5 RV / bit6 CZ / bit4 RA vs MPCore bit5 **SMP** / bit6 parity / bit4 EXCL | force MPCore-safe value (AMP, bit5=0) |
| SCTLR 1176-only bits | entry stub | SBZ/SBO differences | the stub's mask (0x00C0380D) uses only common bits [verified-bin] |
| TTBR bit0/bit2 | entry stub ORs 0x18 (RGN only) | bits 0 and 2 SBZ on MPCore | none needed |

Note that the TRM says an absent encoding is *UNPREDICTABLE*, not guaranteed to
UNDEF. The M1 test build probes each one under an undefined-instruction
handler (§6).

### 3.5 SMP and coherency
- MPCore boots in **AMP mode** (ACTLR bit5=0). A single core runs as a uniprocessor if memory is mapped **S=0** ([MPC] §5.2.5).
- The kernel's boot supersections use S=0 [verified-bin]. The pmap templates are checked in M2.
- S=1 Normal memory in AMP mode is uncached, and LDREX/STREX on it needs an external monitor ([MPC] §8.3). **Keep S=0.**
- Cache and TLB maintenance is **not broadcast** between cores ([MPC] ID_MMFR3=0; Linux `smp_plat.h`). That's fine while core 1 stays parked in boot11.

### 3.6 Instruction set
- Per LLVM, ARM1176 is ARMv6KZ and MPCore is ARMv6K; both have LDREX{B,H,D}/CLREX. On ARM1176, WFE/SEV are NOPs.
- The kernel targets ARMv6 and runs on MPCore as-is.
- PMU (c15,c12,0–3) uses identical encodings on both.

### 3.7 Errata ([ERR])
- **720247**: speculative fetches to random addresses (r0p0–r2p0). The workaround is XP=1 plus XN on device mappings. XNU already uses XP=1; its device PTEs get checked in M2.
- 351422, 364261, 452050 affect early revisions only. The 3DS revision is unknown, and M1 prints MIDR.
- **341802**: flush the BTAC (c7,c5,6) on CONTEXTIDR change. The kernel uses c7,c5,6, but whether the context switch does is checked in M2.

### 3.8 TCM
MPCore has no TCM, and the kernel uses no c9 TCM registers [verified-bin].

### 3.9 darwin-on-arm (winocm XNU) on these cores [verified-src]
- [darwin-on-arm/xnu](https://github.com/darwin-on-arm/xnu) supports S5L8720X, S5L8900X, RealView EB v6 and Raspberry Pi as ARMv6 targets.
- It is hard-wired to `__ARM_PROCESSOR_CLASS_11JZFS__` (1136/1176).
- `cpufunc-v6.s` uses MCRR range ops and has `#if 0 /* TODO: ARM11 MPCore */`.
- I found no record of it booting a stock iOS userland on ARMv6.

---

## 4. S5L8720 drivers inside the 4.2.1 kernelcache [verified-bin]

There are 99 bundles in total, listed by `make fwprep` in `build/staging/kernelcache.txt`.

**SoC and board drivers** have to be kept from loading (delete their DT nodes) and replaced only where a function is needed:

| Kext | DT node(s) | Function | 3DS action |
|---|---|---|---|
| AppleS5L8720X, AppleS5L8900X | `/arm-io`, power, clkrstgen, gpio | platform expert, clocks, power, GPIO | **replace** (minimal platform driver) |
| AppleARMPL192VIC | vic, edgeic | interrupt controller | **replace**: MPCore GIC driver (IOInterruptController) |
| (kernel pexpert timer) | timer `timer,s5l8720x` | system tick | **replace**: MPCore private timer (patch pexpert) |
| AppleS5L8900XSerial, (pexpert uart) | uart0/1 | serial console | **replace**: framebuffer / RAM-log console |
| AppleARMPL080DMAC | dmac0/1 | DMA | drop |
| AppleS5L8900XCrypto, AppleKeyStore, AppleEffaceableStorage, AppleImage3NORAccess, IOFlashNVRAM | aes, sha1, pke, prng, spi0/1 nor-flash | crypto, keybag, NVRAM | drop/stub (keybag issues are an M3 risk) |
| AppleS5L8720xFMSS, AppleNANDFTL, AppleNANDLegacyFTL, AppleNANDFirmware, IOFlashStorage | flash-controller0 | NAND root disk | **replace**: SD block device (M3), or RAM disk via `RAMDisk` + DiskImages |
| AppleM2DisplayDrivers, ApplePinotLCD, AppleM2ScalerCSCDriver.n72, (mipi-dsim) | clcd, mipi-dsim/lcd, scaler, tv-out | display (IOMobileFramebuffer provider) | **replace**: 3DS PDC framebuffer driver (M4) |
| AppleMBX | mbx | MBX Lite GPU (GL kernel side) | **replace**: PICA200 (M5) |
| AppleM2H264MPD, AppleMPVDDriver | h264decoder, mpvd | video decode | drop |
| AppleMultitouchSPI, AppleMultitouchSPIZ2F53 | spi4/multi-touch | Zephyr2 touch | **replace**: 3DS TSC touch (M4) |
| AppleM68Buttons | /buttons | home/power/volume | **replace**: 3DS HID/MCU (M4) |
| AppleD1759PMU | i2c0/pmu | PMU, battery, RTC | **replace/stub**: MCU via I2C (battery, RTC) |
| AppleEmbeddedAccelerometer | i2c0/accelerometer | accelerometer | optional (3DS has one on the MCU) |
| AppleEmbeddedLightSensor | i2c1/als | ALS | drop |
| AppleCS42L58Audio, AppleEmbeddedAudio, AppleARMIISAudio, AppleLM48821Amp, AppleCD3272Mikey | i2s0, audio0, spkr-amp, mikey | audio | drop (later: 3DS CODEC/DSP) |
| AppleSynopsysOTGDevice, AppleS5L8720XUSBPhy, AppleEmbeddedUSB, AppleUSBDeviceMux, AppleUSBMike | usb-complex, otgphyctrl | USB device | drop |
| AppleS5L8900XSDIO, AppleBCMWLAN, IOSDIOFamily | sdio | Wi-Fi | drop |
| AppleBluetooth, AppleOnboardSerial, AppleReliableSerialLayer | uart1/bluetooth, uart0/iap | BT, dock serial | drop |
| AppleTetheredDevice, AppleAMC_r2, AppleARM11xxProfile, AppleMicron2020 | tethered, amc, — | misc | drop |

**Generic families that must keep working** (this is the reason to keep Apple's
kernel): IOSurface, IOMobileGraphicsFamily, IOHIDFamily/IOHIDEventDriver,
IOStorageFamily, DiskImages, AppleMobileFileIntegrity, Sandbox,
IOAudio2Family, IONetworkingFamily, IOUSBDeviceFamily, AppleFSCompression,
FairPlayIOKit, and others.

---

## 5. Prior art

| Project | What it is | Kernel | Relevance |
|---|---|---|---|
| [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios) (in the Mac app [Light Touch](https://github.com/samhenrigold/LightTouchMac)) | QEMU machine for n72ap. Boots **stock 4.2.1 8C148 to the home screen**. | Apple's stock kernelcache with its own iBoot-less loader | **Highest.** Proves the iBoot-less boot of this exact kernel. Its `docs/ipod/from-ipsw.md` documents boot_args placement, the boot-args that work, and 8C148 pitfalls (timer, NAND, AES-UID, keybag/data protection, SDIO). Its GL path is a guest **OpenGLES/MBXGLEngine shim that forwards GL calls to the host**. That's the same user-space seam we'd use to put a PICA200 engine behind OpenGLES in M5. |
| [devos50/qemu-ios](https://github.com/devos50/qemu-ios) | iPod touch 1G (iPhoneOS 1.0) and 2G (**iOS 2.1.1**). Runs real SecureROM → LLB → iBoot. | stock | Peripheral models with S5L8720 MMIO maps ([ipod_touch_2g.h](https://github.com/devos50/qemu-ios/blob/ipod_touch_2g/include/hw/arm/ipod_touch_2g.h)). **The MBX is a stub; graphics are software-rendered**, so it does *not* forward GL. Blog: [2G](https://devos50.github.io/blog/2023/ipod-touch-2g-qemu/). |
| [ios-sweet](https://github.com/dipa-studio/ios-sweet) | iOS 16.7 userland on a Redmi Note 10 Pro (SM7150) | **open-source arm64 XNU (xnu-8796)** + own shim bootloader + generated device tree | Same "native port" idea. It works because Apple publishes current arm64 XNU. No such source exists for 2010 ARMv6 XNU-1504. |
| [HackiOS/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | iOS on POCO X5 Pro | open-source xnu-8796.121.2 + own drivers | Same as above. Neither project uses Apple's kernelcache. |
| [darwin-on-arm/xnu](https://github.com/darwin-on-arm/xnu) + [GenericBooter-next](https://github.com/darwin-on-arm/GenericBooter-next) | winocm's XNU port (OMAP3, AM335x, RealView, BCM2835, S5L89xx) | xnu-2050-era source | Reference for boot_args, pexpert and the bootloader layout. No MPCore. Booting a stock iOS userland is unverified. |
| [openiBoot](https://github.com/iDroid-Project/openiBoot) | Linux bootloader for old iDevices | — | **Does not boot XNU.** It only boots Linux or chainloads iBoot (actions.c). I had wrongly assumed it was a reference for the XNU handoff. |
| comex [datautils0](https://github.com/comex/datautils0/blob/master/make_kernel_patchfile.c) | iOS 4-era jailbreak kernel patch generator | — | Patch *recipes* (vm_map_enter/protect, AMFI, debug_enabled, tfp0, sandbox) if the boot-arg route isn't enough |

**Firmware keys** for this build are on
[The Apple Wiki](https://theapplewiki.com/wiki/Keys:Jasper_8C148_(iPod2,1)).
Its `index.php?action=raw` is behind a Cloudflare challenge, but the MediaWiki
`api.php?action=query&prop=revisions…` route works. `fwprep` uses the same keys
via api.ipsw.me and accepts `--keys` for a local file.

---

## 6. Open questions and the tests that settle them

| Question | Test (build) |
|---|---|
| 3DS MIDR revision (which errata apply), CTR, FPSID, ID_MMFR*, ACTLR reset value | M1 build prints all CP15 ID registers on screen and in log.txt |
| Does MPCore UNDEF or silently ignore `c15,c2,4` / `c6,c0,2`? | M1 probe: execute each under an installed undefined-instruction vector and log the result |
| Does XNU's video console draw on a 32 bpp boot_args framebuffer? | M2 build 1: Boot_Video → 240×400 ABGR8 buffer in VRAM; check the top screen |
| Does `/chosen/debug-enabled=1` enable debug boot-args? | M2: boot with `debug=0x8` and see whether panics stop for the debugger or print verbosely |
| Does the PDC scan out the top screen from a VRAM buffer XNU never touches? | M1 build already does this |
| Exact remaining boot_args fields | M2: disassemble `PE_init_platform` readers before the first M2 build |
