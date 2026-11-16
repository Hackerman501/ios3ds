# Kernel strategy: option A vs option B

**Status: option A approved on 2026-11-16 ("continue your work"). M1 and M2 are implemented following this plan.**

## The options
- **A. Binary-patch Apple's 4.2.1 kernelcache.** Keep the stock
  `xnu-1504.58.28` kernel and its generic kexts. Patch the few ARM1176-only
  instructions, generate a 3DS device tree so the S5L8720 drivers never match,
  and add our own replacement drivers.
- **B. Community open-source XNU.** Start from winocm's port
  ([darwin-on-arm/xnu](https://github.com/darwin-on-arm/xnu), xnu-2050
  lineage), port it to the 3DS, and make it run the 4.2.1 userland.

## What the M0 analysis found
(Details in [research.md](research.md) and [re-log.md](re-log.md).)

1. **The iBoot→kernel contract is small and flexible.**
   - The entry stub only needs r0 = physical boot_args.
   - It takes physBase, virtBase and memSize from that struct. Nothing is
     hard-coded to S5L8720 DRAM.
   - The page tables go at `topOfKernelData`.
   - 3DS FCRAM at 0x20000000 meets every constraint: 16 MB alignment,
     supersections, and 16 KB TTBR alignment.
2. **CPU differences are a short, known list.**
   - Kernel and kexts: no SMC, no VBAR, no MCRR range ops, no TCM.
   - Exactly **5 instruction sites** need attention: 2× peripheral-port remap,
     2× IFAR, and the ACTLR setter.
   - Cache geometry is read from CTR, except for one boot loop. That loop's
     16 KB assumption matches the 3DS anyway.
3. **The SoC dependencies are mostly kexts matched via the device tree.**
   - We write the device tree, so we decide which Apple drivers load.
   - What remains SoC-specific *inside* the kernel proper is a pexpert table
     keyed on `/arm-io device_type`, which provides the timer, UART and
     clock constants. Those few functions get patched.
4. **The kernel exports 4,391 KPI symbols** (LC_SYMTAB). New drivers can be
   linked against it the same way Apple's kexts were.
5. **The userland depends on Apple's kernel-side families.**
   - IOSurface, IOMobileGraphicsFamily (IOMobileFramebuffer user client),
     IOHIDFamily, AppleMobileFileIntegrity and Sandbox are all closed source
     and inside the kernelcache.
   - SpringBoard, backboard, GraphicsServices and CoreSurface talk to them
     through private user-client interfaces.
6. **darwin-on-arm doesn't support ARM11 MPCore.**
   - It is ARM1136/1176-only.
   - It uses MCRR range ops that MPCore lacks.
   - Its MPCore ASID flush is an `#if 0 TODO`.
   - I found no record of it running a stock iOS userland on ARMv6.
   - It does contain an S5L8720X platform file. That's useful *reference*
     material for either option.

7. **Prior art lines up with this.**
   - [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios) boots
     this *exact* stock 8C148 kernelcache to SpringBoard. It does so without
     iBoot, from its own loader, boot_args and device tree. That shows Apple's
     binary kernel tolerates a foreign bootloader, and its notes list the
     8C148-specific pitfalls.
   - ios-sweet and HackiOS take the "open-source XNU" route (option B's
     approach). They can, because Apple publishes current **arm64** XNU.
     Nothing comparable exists for 2010-era **ARMv6** XNU-1504.
   - Option B here would mean darwin-on-arm, a 2013 hobby port with no MPCore
     support. It is not Apple's code.

## Comparison

| | **A: patch Apple's 4.2.1 kernel** | **B: darwin-on-arm XNU** |
|---|---|---|
| Userland binary compatibility | **Exact by construction.** Same syscall and Mach trap tables, commpage, signal frame and thread state layout, and the same IOKit user clients the 4.2.1 frameworks were built against. | **Unknown and large.** 2050 vs 1504 differences in syscalls, traps, commpage and the shared-region ABI. Every Apple-private user client (IOSurface, IOMobileFramebuffer, HID event system, AMFI/Sandbox MAC hooks, IOKit property shapes SpringBoard expects) must be **re-implemented from scratch** against undocumented ABIs. |
| Work to reach M2 (kernel prints and panics) | **Small–medium.** Loader + device tree generator + ~5 instruction patches + console hook (patch `_kprintf`/serial putc into a RAM log, or Boot_Video framebuffer). The timer and interrupt controller can wait until after the first panic. | **Medium.** Port pexpert to the 3DS, fix the MCRR/MPCore cache code, write the GIC/timer/console, and build a 2013-era Darwin toolchain (clang + cctools/ld64, kext tooling). It boots to a panic quickly once it builds. |
| Work to reach M3 (launchd, shell) | **Medium.** A GIC IOInterruptController, a timer, a minimal platform expert, and a block device (or a RAM disk through the built-in DiskImages/`RAMDisk` path). Code signing: AMFI boot-args, or the known 4.2.1 jailbreak patches. | **Medium–large.** The same drivers, plus making 4.2.1 `launchd`/`libSystem`/dyld shared cache run on a different kernel ABI. |
| Work to reach M5 (SpringBoard) | **Large but bounded.** Write leaf drivers that present the *same* interfaces Apple's display/HID/MBX drivers presented (IOMobileFramebuffer provider, HID event service, an MBX replacement or GL engine swap in userland). | **Very large.** Everything in A, plus re-implementing every closed Apple family listed above. |
| Debuggability | Binary only; disassembly-driven. The panic strings carry source file:line, KPI symbols are exported, and open-source xnu-1504.15.3 (Mac, same branch) gives exact struct layouts for the generic code. | Full source, printf anywhere, gdb with symbols. |
| Risk: legal/redistribution | Only patch *recipes* and our own code go in the repo. The patched kernel is generated from the user's IPSW. | Clean: APSL source plus our code. The userland is still Apple's. |
| Evidence it can work | Stock 8C148 kernel already boots without iBoot in samhenrigold/qemu-ios, with the GL user-space seam proven there. | No known stock-iOS userland boot on darwin-on-arm ARMv6. |
| Main risks | (1) Writing IOKit C++ kexts against an undocumented 2010 ABI (vtable layout, `-fapple-kext`, Mach-O kext linking; needs clang + an ld64 port). (2) Hidden SoC assumptions in Thumb pexpert code. (3) The keybag/effaceable-storage dependencies of data protection in M3. | (1) Userland compatibility might *never* be reached for SpringBoard. (2) A large, stale 2013 build system. (3) No MPCore support, so the low-level work is the same as A anyway. |

## Recommendation: **Option A**, with B kept as reference

The project's end goal is Apple's own SpringBoard running on top. Only option
A keeps the hundreds of private kernel↔userland interfaces intact for free.
The analysis shows that A's hardware-facing surface is small: one entry
contract, five instruction sites, a device-tree-driven driver set, and a
documented export table.

darwin-on-arm's real value is reference code: its `pe_s5l8720x.c` and its
cache, pmap and pexpert structure help us understand the closed binary. It
isn't a foundation to build on.

## How A would be carried out

All of the following is applied at build time to `build/staging/kernelcache.macho`.

1. **Patch recipes** in `kernel/patches/*.json`.
   - Each entry: `{offset or byte-pattern, expected original bytes, replacement, reason}`.
   - The applier refuses to patch if the original bytes don't match, so a
     different IPSW can't be silently mis-patched.
   - The first set:
     - NOP `c15,c2,4` ×2
     - IFAR → `LR_abt−4` ×2
     - ACTLR setter forced to an AMP-safe value
     - `_kprintf`/console putc → RAM ring buffer hook
2. **Device tree generator** (`kernel/mkdt.py`).
   - Start from the n72ap template.
   - Delete the S5L8720 hardware nodes.
   - Fill `/memory`, `/chosen/memory-map`, frequencies and `debug-enabled`.
   - Rewrite `/arm-io` for the 3DS.
3. **Our code blob.**
   - Plain C, linked at a fixed physical address just after the kernelcache.
     That region is already covered by the kernel's supersection map, so its VA
     is `PA − 0x20000000 + 0x80000000`.
   - Patched branches in the kernel call into it: timer, IRQ dispatch, console.
   - This avoids needing a working kext toolchain for M2.
4. **IOKit kexts** (M3+): the GIC interrupt controller, platform, SD storage,
   framebuffer and HID.
   - Built with clang `-target armv6-apple-ios -fapple-kext` against
     open-source xnu-1504 IOKit headers.
   - Linked and injected into `__PRELINK_INFO` by our own prelinker script.
   - The toolchain decision (cctools-port ld64 vs a custom Mach-O linker) is
     made at M3, with a spike first.

## If you choose B instead
The M1 loader is identical for both options. Only M2 changes:
- build darwin-on-arm for an `ARM_3DS` config;
- fix MPCore cache ops;
- write the 3DS pexpert.

I'd still expect to hit the userland-ABI wall at M3/M5.

## Decision needed
Approve **A** (recommended) or **B**. After approval: M1 (3DS payload), then M2.
