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
