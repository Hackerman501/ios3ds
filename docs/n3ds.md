# New 3DS extras: 804 MHz, L2 cache, C-stick and ZL/ZR

On a New 3DS, New 3DS XL or New 2DS XL, ios3ds can use the faster CPU clock, the L2 cache, the C-stick and the ZL/ZR buttons. Each extra has a `boot.cfg` switch. On an Old 3DS, 3DS XL or 2DS nothing changes: the model check reads `CFG11_SOCINFO` and stops there.

| Switch | Default | What it does |
|---|---|---|
| `n3ds_clock=1` | **on** | ARM11 at 804 MHz instead of 268 MHz (3×) |
| `n3ds_l2=0` | **off** | the 2 MiB L2C-310 L2 cache, write-through (experimental, section 2) |
| `n3ds_hid=1` | **on** | read the C-stick / ZL / ZR chip (section 3) |

Still not used on any model: CPU cores 1–3 (XNU 1504 is uniprocessor), the extra 128 MiB of FCRAM, and the extra QTM RAM.

**None of this has run on a New 3DS yet.** Section 5 lists what the first hardware log must show.

## 1. 804 MHz

### Where and how
The ARM11 boot stub (`boot/arm11/main.c`, `a11_n3ds`) switches the clock before XNU starts. At that point the MMU and caches are off and nothing else runs on the ARM11. The ARM9 asks for it with the mailbox command `A9CMD_N3DS` (`n3ds_setup` in `boot/arm9/main.c`), right before the countdown.

The sequence is the one in libn3ds `PDN_core123Init()` / `PDN_setSocmode()` and linux-3ds `firm_linux_loader/arm11/source/smp.c`:
1. Read `CFG11_SOCINFO` (0x10140FFC): bit 1 is LGR1, set on every New 3DS; bit 2 is LGR2, set on every retail New 3DS. Without bit 1, stop here.
2. GIC: turn on the CPU interface and the distributor, disable every interrupt, then enable interrupt 88 (the PDN "socmode changed" interrupt) for core 0 at priority 0.
3. Write `PDN_LGR_SOCMODE` (0x10141300) back to itself, which clears an acknowledge bit that a bootloader may have left set.
4. Set `PDN_LGR_CNT` (0x10141304) to 0x101: bit 8 powers the L2 cache controller, bit 0 enables the memory extension. Then wait about 400 cycles.
5. Write mode 5 (LGR2 804 MHz; mode 3 = 536 MHz on LGR1-only chips) to `PDN_LGR_SOCMODE`, and `WFI` until bit 15 (ack) is set.
6. Write the register back to acknowledge, and clear GIC pending 88.
7. Set `CFG11_GPU_N3DS_CNT` (0x10140400) to 3 (N3DS mode + texture fix), as libn3ds does for any non-CTR mode.

The `WFI` loop has a bound: the MPCore private timer (GIC 29) wakes the core every 10–30 ms, and after about 1 s without an ack the stub gives up and reports `N3RES_TIMEOUT`. With `n3ds_clock=0` the stub puts a New 3DS back into plain CTR mode (mode 0), as libn3ds does by default, if something else had left it faster.

### Timers
XNU's time base is the MPCore watchdog and private timer at PERIPHCLK/128 = 1,047,311 Hz (`kernel/dtgen.py` `timebase-frequency`, `kernel/shim/entry.S`). PERIPHCLK is CPU/2, so at 804 MHz it is 402 MHz. Keeping 1,047,311 Hz would need a divider of 384, but the prescaler has only 8 bits (at most /256).

So the timers run at /192 = 2,094,622 Hz, exactly twice the time base, and the shim halves every count:
- `get_abstime`: the 64-bit count is shifted right by one, with a two-instruction `lsr`/`rrx`, before XNU's offset is added. It stays stackless.
- `get_decrementer`: the count is shifted right.
- `set_decrementer`: the count is shifted left, capped at 2^30.
- `shim_now32()` replaces the raw watchdog reads in audio.c, cam.c and pica.c. It returns the low word of the shifted 64-bit count, so a 32-bit wrap still works.

Everything else keeps 1,047,311 Hz: the device tree, audio rates, camera and GPU timeouts, and the ARM9's heartbeat.

The stub doesn't assume PERIPHCLK; it measures it. The watchdog runs with prescaler 0 for 1309 ticks (20.0 ms) of the ARM9's fixed 65,457 Hz timer (`cal_state` handshake in the mailbox). Then the stub rounds to a multiple of 134,055,928 Hz:

| Ratio | Prescaler | Shift |
|---|---|---|
| 1 | /128 | 0 |
| 2 | /256 | 0 |
| 3 | /192 | 1 |

If the ARM9 doesn't take part, the stub uses the ratio from the clock mode. The result goes into shim word +0x0C (`cfg_n3ds`, `N3RES_*` in `boot/common/ios3ds.h`). With 0 there, as on an Old 3DS or with an older ARM9, the shim behaves exactly as before.

### Other clock effects
- Cycle-counted waits on the ARM11 get 3× shorter. That's only `blk.c`'s 200 spins before it sleeps, which is harmless. Everything else waits on the time base (`IODelay`, ticks).
- The device tree still says 268 MHz CPU (`hw.cpufrequency`). Nothing in XNU times itself by it.
- The ARM9 (134 MHz), the GPU, the LCD and the ARM9 timers are on separate clocks and don't change.

## 2. The L2 cache (`n3ds_l2=1`, off by default)

### Why it is off by default
1. **The L2 is an outer cache, and XNU doesn't know it exists.** The S5L8720 has none, so XNU's cache maintenance stops at L1. XNU maps RAM as TEX=000 C=1 B=1, outer and inner write-back, no write-allocate: the kernel's 16 MiB supersections are built at kernelcache file offset 0x660a0. With the L2 on, every agent that is not the ARM11 CPU sees stale RAM unless someone cleans or invalidates the L2 for it. Those agents are the ARM9 (all mailboxes), the GPU (arena, mirror, present), and CSND and the cameras through the shim.
2. **QEMU models no caches.** It can show that the maintenance calls happen, but not that they are enough.
3. Some reads of ARM9-written memory have no invalidate at all, because L1 eviction was good enough. `st_panic_req` (SELECT+START) is one; a 2 MiB L2 holds such lines far longer.

### What `n3ds_l2=1` does
The stub enables the L2C-310 at 0x17E10000 before XNU starts, in a mode that avoids the worst case:
- **Write-through:** debug control register 0xF40, DWB = 1. The L2 never holds dirty data, so every L1 clean or eviction still reaches RAM. ARM11 → ARM9/GPU paths need nothing new.
- **Shared attribute override:** aux control bit 22. Non-cacheable shared accesses don't look up the L2.
- Invalidate all ways, sync, clear interrupts, then enable. If the write-through bit doesn't stick, the L2 stays off.

That leaves one direction: RAM the ARM9 or the GPU wrote, read by the ARM11. Every shim helper that invalidates L1 now first calls `shim_l2_inv` / `shim_l2_inv_kva` (`entry.S`), which invalidates the lines by physical address (L2C 0x770) and syncs (0x730). Because the L2 is write-through, invalidating a clean line can't lose data, so a wrong guess only costs time.

### Path audit
| Path | ARM9/GPU → ARM11 maintenance | File |
|---|---|---|
| md0 mailbox + 32 KiB bounce (block reads) | `dinval` → L2 inv | `kernel/shim/blk.c` |
| input mailbox | `input_dinval` → L2 inv | `tools/ios3ds-input/shim/input.c` |
| MCU mailbox (battery, RTC, accelerometer, gyro, power) | `dinval` → L2 inv | `tools/ios3ds-mcu/shim/mcu.c` |
| network mailbox (Wi-Fi frames) | `net_dinval` → L2 inv | `tools/ios3ds-wifi/shim/net.c` |
| audio mailbox, mic ring | `dinval` → L2 inv | `kernel/shim/audio.c` |
| camera mailbox | `dinval` → L2 inv | `kernel/shim/cam.c` |
| GPU mirror mailbox, GPU arena (READ/TRANSFER/FILL, self-tests) | `dc_inval`, `p_from_gpu` → L2 inv by the arena's PA | `kernel/shim/pica.c` |
| arena allocation | whole arena L2-invalidated once | `kernel/shim/pica.c` |
| PRESENT into a user surface | L2 inv of the surface's PA before the GPU writes | `kernel/shim/pica.c` |
| log ring, IRQ samples, probe snapshot, mailbox writes, PCM ring, command lists | ARM11 → ARM9 only: write-through covers them | `entry.S`, etc. |
| page-table walks | TTBR0/1 = base \| 0x18 (outer WB), so walks go through the L2 and see L1 cleans | kernelcache 0x66bc0 |
| `st_panic_req` (SELECT+START) | **none**: may react late or not at all with L2 on | `entry.S` |
| `/vram`, fake S5L pages (AXI WRAM), IO | device or uncached mappings: L2 not involved | |

Still unverified on hardware:
- whether the ARM11 may write the debug control register (no TrustZone on the MPCore, so it should be secure);
- L2C-310 errata on the 3DS's revision;
- the reset latencies;
- whether any XNU-internal path reads memory written by someone else without going through the shim.

QEMU shows the calls happen: `qom-get /machine l2c-inv-lines` grows during boot (section 4).

## 3. C-stick and ZL/ZR

### The chip
I2C bus 3 (0x10148000), address 0x54. It is libn3ds `I2C_DEV_EXTHID` (17), 3dbrew "I2C Registers" device 17, and GBATEK "3DS I2C New3DS C-Stick and ZL/ZR-Buttons".
- It has no register index: a read transfer returns a report from byte 0.
  - byte 0: status 0x80..0x83
  - byte 1: buttons, ZL = bit 2 and ZR = bit 1 (3dbrew)
  - bytes 2–3: X/Y, about ±0x1x
  - byte 4: 0xFF
  - bytes 5–6: X/Y, about ±0x7x (right and up are positive)
- Reading more than 0x1A bytes can hang the chip, so we read 7.
- The bus needs CNTEX "wait while SCL is held low"; `i2c_init` sets it.

The ARM9 (`boot/arm9/exthid.c`, `i2c_read_noreg`) reads the chip at 60 Hz once the model check found a New 3DS. ZL and ZR become input-mailbox button bits 14 and 15, which are events through the ring. The stick goes into `inmbox_t.ext`, a level that the daemon reads on every poll (`boot/common/input_proto.h`). The first report and every ZL/ZR change are logged; with `diag=1`, every changed report is logged raw.

### In iOS
`tools/ios3ds-input/daemon/n3ds.h` is pure logic, tested by `tests/test_n3ds.c` in `make test`:

| New 3DS | iOS |
|---|---|
| C-stick flick ← / → | previous / next page, like the D-pad |
| C-stick flick ↑ / ↓ | scroll by about 0.6 screen. The finger is held still before it lifts, so there is no fling |
| **ZL** | scroll to the top: a tap on the status bar, the iOS gesture for it |
| **ZR** | screenshot to the Camera Roll: POWER pressed while HOME is held, paced like a person (HOME, +150 ms POWER, +100 ms POWER up, +100 ms HOME up) |

ZL is not the app switcher: the iPod touch 2G has no multitasking in iOS 4, and in QEMU two HOME presses did no more than one.

- A flick counts once it passes 64 of about ±127 on its dominant axis. The stick must come back inside 32 before the next flick.
- The circle pad keeps its fine scrolling.
- Kill switch: `non3ds` in the daemon's ProgramArguments, or `n3ds_hid=0` in boot.cfg.

Unverified: the axis signs, the range, the ZR bit, and the 45° axis rotation that some reverse-engineering notes mention. Check the `[exthid]` log lines with `diag=1`.

## 4. QEMU: `-M nintendo3ds,new3ds=on`

`emu/run.py --new3ds`, `emu/soak.py --new3ds`, or `IOS3DS_NEW3DS=1` for every emu script (`emu/qemu/hw/arm/nintendo3ds.c`).

| Part | Model |
|---|---|
| SOCINFO | `CFG11_SOCINFO` / `CFG9_SOCINFO` = 7 (Old model: 1) |
| `PDN_LGR_SOCMODE` | a mode write completes 20 µs later: bit 15 set, GIC 88 raised until the guest writes bit 15 back. The MPCore PERIPHCLK becomes CPU/2 of the new mode, 402 MHz for mode 5 (QEMU patch 0001 now re-reads it on every timer control write) |
| `PDN_LGR_CNT`, `CFG11_GPU_N3DS_CNT` | stored |
| L2C-310 at 0x17E10000 | registers only (id r3p3, aux 0x02090000 = 16 × 128 KiB); maintenance ops finish at once and are counted. On the Old model the address is unmapped and aborts, like an unknown register |
| C-stick / ZL / ZR | I2C bus 3, 0x54, the report above. Keys: **Z** = ZL, **N** = ZR, keypad **8/2/4/6** = C-stick. QOM: `cstick-x`, `cstick-y` (−127..127), `exthid-buttons` (4 = ZL, 2 = ZR), `exthid-reads` |
| QOM, read only | `n3ds-socmode`, `n3ds-socmode-switches`, `n3ds-lgr-cnt`, `n3ds-gpu-cnt`, `l2c-ctrl`, `l2c-debug`, `l2c-inv-lines`, `l2c-inv-ways`, `l2c-syncs` |

Measured speed-ups can't come from QEMU: TCG runs at its own pace, and with `--icount` the instruction clock doesn't change with the modelled CPU clock.

## 5. First hardware run: what the log must show

In `sd:/ios3ds/log.txt`:
1. **Model and clock:**
   ```
   [a11] New 3DS: SOCINFO 7, SOCMODE 5, LGR_CNT 101, PERIPHCLK 4021xxxxx Hz, timer /192 >>1
   [n3ds] model: New 3DS (CFG11_SOCINFO 7), ARM11 804 MHz, PDN_LGR_SOCMODE 5, LGR_CNT 101
   [n3ds] MPCore timers: PERIPHCLK 402.xx MHz measured, prescaler /192, tick shift 1 -> 10473xx Hz
   ```
   - SOCINFO may read 6 or 7; bit 1 is what matters.
   - A PERIPHCLK near 134 MHz with SOCMODE 5 means the timers don't follow the CPU clock. The stub then keeps /128 with no shift, which is also correct.
   - `CLOCK SWITCH TIMED OUT` means the handshake failed: report it, and use `n3ds_clock=0` for now.
2. **Time keeps time:** after boot, the `[hb] clock: arm11 watchdog +N ms vs arm9 timer +M ms (ratio R/1000)` lines must show R ≈ 1000. 3000 or 500 means the tick scaling is wrong.
3. **Boot time:** the `[boot] … s:` stamps, and the time from `jumping` to the home screen, against the same card on 268 MHz (`n3ds_clock=0`). On the Old 3DS the home screen takes about 60 s. The md0 reads from SD are served by the ARM9 at its own speed, so expect well under 3× overall.
4. **L2 (only with `n3ds_l2=1`):** `[n3ds] L2 cache: on, write-through (L2C-310 id 410000cX aux … ctrl 1)`. Then use iOS normally: Safari, Settings, Camera, sound. Any data corruption, a garbled screen or a hang that `n3ds_l2=0` doesn't have is an L2 coherency bug. Report it with the log.
5. **C-stick:** `[exthid] C-stick + ZL/ZR at I2C 3:54: 80 …` at boot. With `diag=1`, move the stick in each direction and press ZL and ZR: the raw lines tell us the signs, the range and the ZR bit.
6. **GPU:** the `[gpu] mirror self-test` and `[gl] pica: GPU self-check` lines must still pass, because the GPU now runs in N3DS mode (`CFG11_GPU_N3DS_CNT` = 3).
