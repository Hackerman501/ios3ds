# Real-hardware logs (Old 3DS, MIDR 410fb025)

`sd:/ios3ds/log.txt` from each run, oldest first. Every run stops right after `iBoot version: ios3ds-loader`. The monitor's bottom-screen probe rows were read off photos (not committed):

| Run / build | Bottom-screen probes after the hang |
|---|---|
| run3 | `shim i1 t0 o0 ...`: no timer IRQs |
| run4 | `abs 34 w000000a5 c093`, `dec 1 l10473 @8006953?` |
| run5 (SCU+SMP on) | `ACTLR 0000002f`, `abs 34 w00000ca4 c093`, `dec 1 l10473 @800695d3`, `shim i1 t0 o0 L0 d0 v0 j0` |
| run6 (`cc0ccbd` probes) | `sctlr 00c5787d`, `irq 0 pt 1c29 c7f05 s0`, `cc1 pmf0 hp3ff dc1 act0`, `en2000ffff/00000400 pd00000000/00000000`, `abs 34 w00000cc4 c093`, `dec 1 l10473 @800695d3`, `shim i1 t0 o0 L0 d0 v0 j0` |

run6.txt was copied off the card on 2026-11-17, before the fixed build (`3c57ea7`) went on.

`qemu-gdb/` contains the gdb scripts and output that stop QEMU at the same 34th/35th clock read, for comparison. Read 34 has CPSR `a0000093`; read 35 is in a new thread with IRQs enabled, and `dec` = 4 by then.

**Solved (2026-11-17):** QEMU with `--icount 2` reproduces these values exactly. The cause was the clock base `rtclock+0x30` = 0: `shim_get_abstime` returned 0 before the shim was initialised, and a calendar conversion then spun with IRQs masked. See [re-log §25](../re-log.md). The g34/g35 comparison below predates this: in plain QEMU the first tick fires before read 2, so it compared runs with different timing.

From the next build on, the monitor writes all probe values to `log.txt` (`[probe]` lines) after ~5 s without a clock read or console output.

## run7 (`3c57ea7`, clock-base fix)
Past the old hang: IOKit, audio CODEC init, display mirror on, `BSD root: md0`, and `launchd[1] has started up`. The log ends there, without a `[probe]` dump, so the kernel was still reading the clock. QEMU next prints launchctl's messages and `Running fsck on the boot volume...`. The next build logs a `[hb]` heartbeat every 10 s while the console is quiet: counters, md0 requests, and IRQ-time PC samples (`u`/`k` = user/kernel, `t` = Thumb).

## run8 (`9867d28`, heartbeat): iOS boots to SpringBoard on the real 3DS
- launchd starts at ~115 s, SpringBoard runs by ~200 s, the input daemon connects at ~320 s.
- Taps arrive (`[input] down …`), Mail was opened (MobileMail pid 32), and volume-down reached iOS (`key ea`).
- Audio: `AppleEmbeddedAudioDevice: could not start DMA: device is not ready`.
- The last heartbeat shows the ARM11 idle at `k80067b34`.
- md0 runs at ~19 requests/s. Every request stalls the kernel until the ARM9 serves it, and the ARM9 spent its time in SD transfers, screen copies, and per-byte, per-sync logging. The next build (run9) targets that.

## run9 (`763f374`, ARM9 performance build)
- SpringBoard and the input daemon are up before the first heartbeat at t=83 s (run8: input at ~320 s). md0 had read 75 MB by then, with no errors.
- `gltest: PASS` on real hardware with the CPU rasterizer, but one offscreen 320x480 frame takes 1089 ms. That is the motivation for the PICA200 offload.

## run10 (`24d2d1c`, IORTC wait 0 s + md0 linger)
- `BSD root` right after the kernel starts. SpringBoard and input were up before the first heartbeat at t=66 s.
- Settings, Notes and the App Store opened. Home (`key 40`) and lock (`key 30`) reached iOS.
- Regression: `gltest: offscreen frame took 0 ms`. With timeout 0 the calendar can be initialised before AppleARMPE is up, and early `gettimeofday` deltas read 0. QEMU reproduced it intermittently. Fixed in the next build by `iortc-wait-2s` (re-log §26).

## run11 (`9748841`, IORTC wait 2 s)
- Healthy boot: the RTC line is back, SpringBoard and input are up before t=82 s, and Notes and search work.
- gltest still reads `0 ms` on hardware, so the 2 s wait did not fix the early-boot `gettimeofday` problem there (it did in QEMU). The next build logs the raw `gettimeofday` result.

## run12 (`21f26fc`, first run of the PICA200 paths)
- Boots as before. The GPU display mirror's self-test failed in all 8 variants (`451182 of 518400 bytes differ`, first byte 0x80 where 0x00 was expected) and the GL self-check failed at the memory fill (`4096 of 4096 words wrong (word 0: 0x5a5aa5a5)`); both fell back as designed (ARM9 copy, CPU GL).
- `GX clock 0x00000000`: Luma never sets GX +0x004; libn3ds/GSP write 0x70100. PPF transfers worked, the fill did not. Analysis and the diagnostics of the next build: docs/pica200.md §12.
- The ARM9 CPU copy costs 300-430 ms per frame on hardware (`41 copies in 30 s, 432.8 ms each`): VRAM access from the ARM9 is far slower than in QEMU, so the GPU mirror is the main performance fix.

## run12 (`21f26fc`, first GPU build) and run13 (`eab9618`, GPU diagnostics)
- iOS boots normally in both runs. The GPU paths fail their self-tests, and the ARM9 CPU copy takes over, as designed.
- The CPU screen copy costs **300–430 ms per frame on real hardware** (`[monitor] CPU screen copy`). That makes it the main reason the UI feels slow.
- run13 `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM FAIL->FAIL; PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT FAIL`:
  - the GPU clock was 0, and our setup now sets `0x70100`;
  - the PSC fill can't write FCRAM;
  - textures that aren't a power of two sample garbage.

  The mirror's 320x480 texture and GL's FCRAM fills therefore fail. Details: docs/pica200.md §12.
- Clock: `gettimeofday` measured 1058/1063 ms for the gltest frame, while `mach_absolute_time` measured 495/412 ms. From the next build on, each `[hb]  clock:` line compares the ARM11 watchdog time base with the independent ARM9 timer. In QEMU the ratio is 1000/1000.

### run13 in detail (analysis after the fact)
- `[gpu-diag] summary: fill VRAM ok->ok, fill FCRAM FAIL->FAIL (as found -> after setup); PPF ok; P3D solid VRAM ok, FCRAM ok; texture POT ok, NPOT FAIL`. The GX clock setup (0 -> 0x70100) changed nothing measurable.
- PSC fills never write FCRAM (traffic counters: `ext-wr +0`); non-power-of-two textures sample wrongly (48x40: 0 of 1920). Those two facts explain both run12 failures. Analysis: docs/pica200.md §13.
- CPU copy: 41-95 copies per ~30 s, 235-430 ms each.

## run14 (`fe45ac4`, all QEMU-verified drivers; card boot.cfg ca_gpu=0, powersave=0, diag=1)
- Ran for an hour with no panic, stall or md0 error. The `[hb] clock` ratio stayed at 1000/1000 the whole time, so the kernel time base is right on hardware.
- **GPU screen copy works on hardware:** `display mirror ACTIVE`, self-test 0 of 518400 bytes off, 11–24 fps for the whole hour, every spot check 0/512. The ARM9 copy before it took ~350 ms per frame.
- **MCU works:** version 12/25, battery 21–22 % charging, 3540–3620 mV, 25 °C, volume slider 63/63, 3D slider. The accelerometer was powered on by Apple's LIS302DL driver, and SpringBoard registered an orientation change (0 → 6). HOME presses arrive.
- **The RTC read 2001-02-04.** The raw 3DS RTC isn't the user's time: Horizon adds an offset kept in the NAND config, which we don't read. New: when the time is set in iOS Settings, the ARM9 saves the offset to `sd:/ios3ds/rtc_offset.txt` and applies it at boot.
- **Audio:** an output stream started and the boot tone played. Whether it was audible isn't in the log.
- **GL on the PICA200: gltest FAIL.** The cube isn't drawn, and scenes 1 and 2 differ a lot from the software rasterizer. The GPU copy and the GPU self-tests pass, so the problem is in the 3D/GL path (depth, vertices or lighting). GL apps still use the CPU rasterizer, so it isn't visible yet.
- Wi-Fi stayed off: no `wifi-fw.bin` on the card (expected).
