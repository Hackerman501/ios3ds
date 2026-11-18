# The 3DS system controller (MCU) in iOS

Status: **implemented and verified in QEMU (`-M nintendo3ds`), not yet run on
hardware.** Section 7 lists what a hardware run will show and which
assumptions it checks.

| Feature | 3DS side | iOS side | QEMU |
|---|---|---|---|
| RTC → date and time | MCU regs 0x30–0x36 (BCD) | `PEGetGMTTimeOfDay` → shim; no IORTC wait | see §6 |
| Battery level, charging | regs 0x0A–0x0F | an `IOPMPowerSource` created by the shim | see §6 |
| HOME, POWER | IRQ regs 0x10–0x13 | input mailbox `INBTN_HOME` / `INBTN_POWER` → ios3ds-input → GSEvents | see §6 |
| Shell close / open | IRQ bits 5/6, reg 0x0F bit 1 | POWER tap (lock) / HOME tap (wake), from the ARM9 | see §6 |
| Volume slider | reg 0x09 (0..63) | ios3ds-mcud → `AVSystemController setActiveCategoryVolumeTo:` | see §6 |
| Brightness | LCD backlight PWM 0x10202240 / 0x10202A40 (ARM11) | SpringBoard's `DisplayBrightness` × `DisplayBrightnessFactor` → ios3ds-mcud → sysctl → shim | see §6 |
| Accelerometer | reg 0x40 (on), 0x45–0x4A (x, y, z) | Apple's AppleLIS302DL driver on an emulated LIS302DL (shim I2C register file) → SpringBoard orientation, UIAccelerometer | see §6.2 |
| iOS shut down / restart | reg 0x20 bit 0 (power off) | `PEHaltRestart` → shim → ARM9 | see §6 |

## 1. Data path

```
 MCU (I2C bus 2, dev 0x4A)
   │  polled by the ARM9 monitor (boot/arm9/mcu.c, i2c.c): IRQ events 30 ms,
   │  sliders 100 ms, RTC 1 s, battery 2 s, accelerometer 50 ms while wanted
   ▼
 mcubox_t  shim region +0x6400 (boot/common/mcu_proto.h)       input mailbox +0x6200
   │                                                          (INBTN_HOME / INBTN_POWER)
   ├── kernel shim (tools/ios3ds-mcu/shim/mcu.c)                         │
   │     PEGetGMTTimeOfDay / PESetGMTTimeOfDay / IORTC wait / PEHaltRestart
   │     IOPMPowerSource ("battery"), refreshed on every sysctl read      │
   │     sysctl kern.ios3ds_mcu (RD), kern.ios3ds_mcuctl (RW)             │
   ▼                                                                      ▼
 /usr/libexec/ios3ds-mcud (tools/ios3ds-mcu/daemon)             /usr/libexec/ios3ds-input
   volume, brightness, accelerometer                              Home / lock GSEvents
```

The ARM9 owns the MCU, as it owns SPI (touch, CODEC) and the SD card. The
ARM11 only touches the LCD backlight registers, which are ARM11-only IO.

## 2. Shared memory

`mcubox_t` (256 bytes) at **shim + 0x6400** (PA 0x20846400), between the input
mailbox (+0x6200..+0x63FF) and the free space below the GPU mirror mailbox
(+0x7F00). Single writer per 32-byte line, as for the other mailboxes:

| Line | Writer | Contents |
|---|---|---|
| 0 | ARM9 | magic `MCU0`, version, state (`OFF/OK/FAILED/DISABLED`), MCU version, `seq`, polls, I2C errors, `valid` bits |
| 1 | ARM9 | `rtc_utc` (Unix time = RTC + `rtc_offset`), `rtc_offset`, raw BCD, read count |
| 2 | ARM9 | battery %, temperature, voltage, flags (reg 0x0F), volume and 3D sliders, event counters |
| 3 | ARM9 | accelerometer x/y/z (raw s16), sample count |
| 4 | ARM11 | `req_seq`, `req_power` (power off), `req_acc` (accelerometer wanted), `backlight` (1 + level last set; 1 = screen off) |
| 5 | ARM9 | `ack_seq` |

## 3. ARM9 driver (boot/arm9/i2c.c, mcu.c)

- **I2C:** GodMode9's ARM9 transfer sequence (also libn3ds'): start + device,
  register, repeated start + device|1 for reads; ACK all bytes but the last,
  last with STOP; NACK → `EN|IRQ|ERROR|STOP` and up to 8 retries. Bus setup
  as libn3ds `I2C_init` (CNTEX = clock stretching, SCL = delays 5/0). Every
  busy wait is bounded (~6 ms): a dead bus never hangs the monitor.
- **Boot** (`mcu_init`, before XNU starts): read the version (regs 0/1);
  no answer → state `FAILED`, one log line, nothing else is touched. Else
  write the IRQ mask (regs 0x18–0x1B, 1 = masked) to unmask POWER press/held,
  HOME press/release and shell close/open, read the IRQ register once to drop
  stale events, then read RTC, battery and sliders, so the kernel finds the
  time when it initialises its calendar.
- **HOME:** press/release IRQs → a level; press and release in the same 30 ms
  poll → one input sample down, then up. A lost release ends after 10 s.
- **POWER:** the MCU has no release event. A press → a 250 ms tap (iOS: lock
  or wake). The MCU's "held" IRQ → POWER down for 6 s, long enough for the
  input daemon's 0.8 s hold detection plus SpringBoard's 2.5 s timer with a
  lagging run loop → the "slide to power off" sheet (a late release is
  ignored once the sheet is up).
- **Shell:** close → POWER tap unless iOS' screen is already off; open →
  HOME tap if it is off. "Off" = the shim's last backlight level is 0
  (`backlight == 1`), so it follows iOS' own screen state. In both cases the
  two LCD backlights are switched through MCU reg 0x22 (0x14 off, 0x28 on),
  because this build's SpringBoard does not blank on a lock press
  (SBDontLockEver).
- **Power off:** on `req_power`, wait 300 ms (the monitor loop drains the
  kernel's last log lines), sync the log, write reg 0x20 = 1.
- **boot.cfg:** `mcu=0` disables all of it; `rtc_offset=N` is added to the
  RTC to get UTC (the 3DS RTC usually runs on local time: e.g. `-3600` for
  CET). iOS' own time zone (Settings → General → Date & Time) is separate.

## 4. Kernel side (tools/ios3ds-mcu/shim/mcu.c, kernel/patches-mcu.json)

RE details: [re-log.md](re-log.md) §33–34.

- **RTC.** `PEGetGMTTimeOfDay` (0x801c84d8) jumps to `shim_mcu_get_gmt`,
  which returns `rtc_utc` (+ a `settimeofday` delta, below). Without a valid
  RTC it runs the original body (`gIOPlatform->getGMTTimeOfDay()`), so the
  fallback is today's behaviour.
- **No IORTC wait.** `IOKitInitializeTime` (0x801a8004) waits for
  `resourceMatching("IORTC")` before `clock_initialize_calendar()`; the literal
  of its `waitForService` call (0x801a8030) now points at `shim_mcu_wait_rtc`.
  With an RTC it returns at once (the matching dictionary is released as
  `waitForService` would): the calendar no longer depends on AppleARMPE being
  registered, which was the reason for the 2 s wait (re-log §26). Without an
  RTC it calls the original `waitForService` with `iortc-wait-2s`' 2 s.
- **Setting the time.** `PESetGMTTimeOfDay` (0x801c84f4) → `shim_mcu_set_gmt`:
  AppleARMPE's version waits 30 s for an IORTC. The 3DS RTC is the console's
  clock and stays untouched; the difference is kept until reboot.
- **Halt.** In `PEHaltRestart` (0x801c98b0) the call
  `gIOPlatform->haltRestart(type)` (0x801c98f6) goes to
  `shim_mcu_halt_restart` instead. It runs after IOKit's shutdown handlers and
  after the file systems were unmounted. Halt (0, 3) and restart (1): request
  `MCU_PWR_OFF`/`MCU_PWR_REBOOT` from the ARM9 and wait; the ARM9 powers the
  console off in both cases (a reboot would end in Luma / the HOME Menu, not
  in iOS). Panic restart, hang and the rest go to the platform as before.
  `pe-halt-skip-pm` skips IOPMrootDomain's halt broadcast just before that
  call: it hung every shutdown in AppleM2CLCD (§6.1).
- **gettimeofday.** `commpage-gtod-off` stops the kernel from publishing the
  commpage time stamp, whose fast path never advanced with our timebase (§8).
- **Battery.** On the first `kern.ios3ds_mcu` read with valid battery data
  the shim creates an `IOPMPowerSource` (`IOPMPowerSource::powerSource()`),
  attaches it to the platform expert and registers it. Every later read
  updates `CurrentCapacity`, `MaxCapacity` (100), `IsCharging`,
  `ExternalConnected`, `ExternalChargeCapable`, `FullyCharged`, `Voltage`
  and `AppleRawBatteryVoltage` and calls `updateStatus()`, which copies them
  into the registry and messages interested clients
  (`kIOPMMessageBatteryStatusHasChanged`). These are the keys SpringBoard
  reads (`IOServiceMatching("IOPMPowerSource")`). `AtCriticalLevel` is never
  set: SpringBoard would shut down immediately; the MCU itself switches the
  console off when the battery is empty.
- **Backlight.** `kern.ios3ds_mcuctl` carries `{acc, backlight}` from the
  daemon. The shim maps the LCD registers with `ml_io_map(0x10202000)`, logs
  the values Luma left, and writes `0x10 + level × (0x8F − 0x10)` to both
  screens' backlight PWM (0 when iOS' screen is off).

## 5. Userland (tools/ios3ds-mcu/daemon/mcud.c)

`/usr/libexec/ios3ds-mcud` (launchd job `com.ios3ds.mcud`, argument `debug`)
polls `kern.ios3ds_mcu` every 100 ms:
- **Volume:** when the slider moved (2 of 63 steps, or to an end):
  `[[AVSystemController sharedAVSystemController] setActiveCategoryVolumeTo:v/63]`
  (Celestial.framework). In between, iOS' own volume buttons (L/R) still work.
- **Brightness:** twice a second it reads `DisplayBrightness` and
  `DisplayBrightnessFactor` from SpringBoard's HID event system
  (`IOHIDEventSystemClientCopyProperty`; SpringBoard keeps its backlight
  state there) and sends `brightness × factor` to the shim.
- **Accelerometer, old route** (only with the `acc` argument; superseded by
  the kernel driver, §6.2): while the screen is on it asks the ARM9 for
  samples and sends `IOHIDEventCreateAccelerometerEvent(x, y, z)` (type 13) into
  SpringBoard's HID event system, normalised so the running mean of |a| is
  1 g (the raw unit does not matter). Axis map `acc=-y,x,-z` (default, an
  argument): iOS x, y, z from the 3DS axes, see §7.
- **Clock check:** logs the same interval measured by the RTC,
  `gettimeofday` and `mach_absolute_time` at ~60 s and ~300 s (§8).

## 6. QEMU model and verification

`emu/qemu/hw/arm/nintendo3ds.c` already had the I2C controllers and an MCU
(IRQs, RTC from the host clock, battery, HOME = `h`, POWER = `p`). Added:
- accelerometer: reg 0x40 bit 0 on, 0x45–0x4A data (1 g = 0x4000), four poses;
- keys: `-` / `=` volume slider down/up by 8, `c` shell close/open,
  `[` / `]` turn the console by 90°;
- runtime state through QOM properties on `/machine` (HMP `qom-set` /
  `qom-get`, `emu/run.py --hmp "t=command;…"`): `mcu-battery`,
  `mcu-charging`, `mcu-volume`, `mcu-shell-open`, `mcu-orient`, and the
  read-only `lcd-backlight`;
- LCD backlight registers 0x10202240 / 0x10202A40 (reset 0x5F); once the
  guest writes them, the emulated screens are dimmed accordingly
  (0x8F = full).

### QEMU results (2026-11-18)

Runs with `emu/run.py` / `emu/tap.py`; artifacts in `build/emu-r*` (not
committed: the screenshots show Apple's UI).

| Driver | Evidence |
|---|---|
| RTC | `[mcu] version 13/38: RTC 2026-11-17 23:21:31 Tue (offset 0 s) = Unix 1794957691`, `ios3ds-mcu: 3DS RTC present: not waiting for IORTC`, `ios3ds-mcu: calendar from the 3DS RTC: 1794957691`; the "RTC did not show up" line is gone. Status bar 3:26 PM, calendar icon "Tuesday 17" (host 23:26 UTC; iOS zone US/Pacific). Same in `--icount 2` |
| gettimeofday | before: `tv 1794957716.452931 -> 1794957716.452931; mach_absolute_time … = 212 ms`; after `commpage-gtod-off`: 181 ms vs 182 ms (plain), 2254 vs 2254 ms (icount); `[mcud] clocks after ~60 s: RTC +153 s, gettimeofday +154086 ms, mach_absolute_time +154084 ms` |
| Battery | `ios3ds-mcu: battery 100%, charger, 4200 mV: IOPMPowerSource registered`; full battery with bolt → `qom-set mcu-charging off`, `mcu-battery 50` → half battery without bolt; `mcu-battery 15` → SpringBoard's "Low Battery, 20% of battery remaining" alert |
| HOME | key `h` → `[input] key 40 down/up` → Spotlight (Home on page 1) |
| POWER | `p` tap → `key 30` (lock); `p` held 5 s → `[mcu] POWER held: iOS power-off slider` → "slide to power off" sheet |
| Shutdown | sliding the sheet → `launchd: System: About to call: reboot(RB_HALT).` → `[mcu] iOS shut down: log synced, powering off through the MCU` → the QEMU model's reg 0x20 write ends QEMU (§6.1) |
| Shell | `c` → `[mcu] shell closed: locking iOS (POWER tap); LCD backlights off` (both screens black); `c` → `[mcu] shell opened; LCD backlights on` |
| Volume | `=` twice → `[mcud] volume slider 40/63 -> iOS volume 0.63`, `48/63 -> 0.76` |
| Brightness | `[mcud] iOS brightness 0.50 x factor 1.00 -> LCD backlight 500/1000`, `ios3ds-mcu: LCD backlight as Luma left it: top 0x5f, bottom 0x5f`, `qom-get lcd-backlight` = 79 (0x4F); the screenshots are dimmed accordingly |
| Accelerometer | `[mcud] accelerometer raw -16384 0 0 -> iOS -0.00 -1.00 -0.00 g`, after `]` `]`: `16384 0 0 -> iOS 0 1.00 0`; SpringBoard's state (§6.2) |

### 6.1 Shutdown hung in AppleM2CLCD before reaching the platform

The first shutdown in QEMU never got to `PEHaltRestart`'s platform call: the
CPU idled after `reboot(RB_HALT)`. A SELECT+START thread dump showed launchd's
thread in `PEHaltRestart+0x3e` → `IOPMrootDomain` halt/restart broadcast →
IOMobileGraphicsFamily → AppleM2CLCD (`disable_backlight`, "Where is my
backlight?") → AppleARMPlatform function → AppleS5L8720X+0x37cc
`IOCommandGate::commandSleep`, waiting for an interrupt from S5L backlight
hardware the 3DS does not have. So every iOS shutdown used to hang forever.
`pe-halt-skip-pm` skips that broadcast (file systems are unmounted by then
and md0 writes are synchronous); the platform call then reaches the shim. The
ARM9 waits 300 ms after the request so the kernel's last lines reach the log,
then writes MCU reg 0x20.

### 6.2 Accelerometer: Apple's driver on an emulated LIS302DL

First attempt (still in ios3ds-mcud as `acc`): IOHID accelerometer events
sent by a client. SpringBoard ignores them, as it ignores client keyboard and
touch events (re-log §37: hardware breakpoints on its type-13 handler never
hit). SpringBoard only takes accelerometer data from a kernel HID service.

Now the n72's own accelerometer node `/arm-io/i2c0/accelerometer` stays in
the device tree (`tools/ios3ds-mcu/dt_mcu.py`). Apple's
AppleEmbeddedAccelerometer (class AppleLIS302DL) starts on it, and the shim
answers its I2C transfers (`shim_mcu_iic`, called first by
`kernel/shim/audio.c`'s `shim_iic_transfer`). Details in re-log §39:
- `WHO_AM_I` (0x0F) reads 0x3B.
- `CTRL_REG1` power-up bit → the shim asks the ARM9 for samples (`req_acc`).
- The `CTRL_REG2` BOOT bit reads back cleared (the driver panics after 500
  tries otherwise).
- `OUT_X/Y/Z` (0x29/0x2B/0x2D) are the newest MCU sample in iOS axes. They are
  scaled so the running mean of |a| is 55 counts (1 g at the driver's
  18 mg/count), with z negated for the DT's `orientation` 4.
- The data-ready interrupt (GPIO 0x63) never fires. The driver polls with its
  own timer at the `ReportInterval` that SpringBoard sets while an app that
  rotates is in front.

QEMU (`build/emu-a2`, Safari opened by a tap, then
`qom-set /machine mcu-orient 1 / 3 / 0`):
```
ios3ds-mcu: accelerometer read reg 0x0f len 1                       (probe at boot)
ios3ds-mcu: accelerometer powered on by AppleLIS302DL (CTRL_REG1 0x47)
launchd[31] Builtin profile: MobileSafari (sandbox)
ios3ds-mcu: accelerometer sample #1: 0 -55 0 counts (|a| = 16384 MCU units)
[mcud] SpringBoard device orientation 0 -> 1
[mcud] SpringBoard device orientation 1 -> 4      (pose 1: landscape, Home button left)
[mcud] SpringBoard device orientation 4 -> 3      (pose 3: landscape, Home button right)
[mcud] SpringBoard device orientation 3 -> 1      (pose 0: portrait)
```
Safari's bookmarks view turns to landscape, to the other landscape, and back
to portrait in the screenshots. SpringBoard and the home screen behave as
before (the home screen does not rotate on an iPod touch).

Kill switch: `mcu_acc=0` in boot.cfg. The shim then answers the driver's
probe with kIOReturnNoDevice, so no accelerometer service exists. Without a
working MCU it does the same.

## 7. Hardware assumptions (unverified) and the log lines that check them

Nothing here has run on a 3DS. Each assumption, and the log line that
settles it on the first hardware run:

| Assumption | Source | Log line to read |
|---|---|---|
| The ARM9 can drive I2C bus 2 (0x10144000) and the MCU answers at 0x4A | Luma/GodMode9 talk to the MCU from the ARM9 | `[mcu] version xx/yy: RTC …` (good) or `[mcu] no answer on I2C bus 2 …` (then everything behaves as before) |
| RTC regs 0x30-0x36 are BCD sec/min/hour/weekday/day/month/year-2000 | libn3ds, GodMode9 | the RTC date in that line; the iOS clock. `rtc_offset` in boot.cfg if the RTC runs on local time |
| Battery: 0x0B percent, 0x0D voltage in 20 mV, 0x0A °C; 0x0F bit 3 charger, bit 4 charging, bit 1 shell open | 3dbrew, GodMode9 (charging bit) | `[mcu] battery 85% 3950 mV 27 C, charger, charging, …`; the battery icon |
| Writing the IRQ mask (0x18) unmasks HOME/POWER/shell events; reading 0x10 clears them | libn3ds `MCU_init` | `IRQ mask set`; `[mcud] … irq 0000000c home 1 …` after pressing HOME |
| POWER "held" (bit 1) comes while still pressed, early enough before the MCU's own forced power-off | 3dbrew | `[mcu] POWER held: iOS power-off slider` and the slider sheet |
| Volume slider 0x09 is 0..63 | libn3ds | `[mcud] volume slider N/63 -> iOS volume …` while moving the slider |
| 3DS LCD backlight PWM at 0x10202240 / 0x10202A40, 0x10..0x8F a safe range | libn3ds `lcd.h`, Luma's presets (0x26..0x5F) | `ios3ds-mcu: LCD backlight as Luma left it: top 0x.., bottom 0x..`; does the iOS brightness slider change the screens? |
| MCU reg 0x22 bits 2-5 switch the backlights | libn3ds `MCU_LCD_PWR_*` | closing the shell darkens both screens: `[mcu] shell closed: …; LCD backlights off` |
| Accelerometer: reg 0x40 bit 0 enables it, 0x45-0x4A are x/y/z s16; axes as in the QEMU model (x right, y hinge, z out of the lower screen) | 3dbrew, libn3ds | `[mcu] accelerometer on`; `ios3ds-mcu: accelerometer sample #1: x y z counts (|a| = …)` (about 55 on the gravity axis, `|a|` = the MCU's 1 g); `[mcud] SpringBoard device orientation …` while turning the console in Safari. Wrong axes: the map in `acc_sample()` (tools/ios3ds-mcu/shim/mcu.c); off: `mcu_acc=0` |
| Reg 0x20 bit 0 powers the console off | Luma `mcuPowerOff` | `[mcu] iOS shut down: log synced, powering off through the MCU` as the last log line, then the console is off |

Kill switch: `mcu=0` in `sd:/ios3ds/boot.cfg` (the MCU is never touched; clock at 1970, no battery, no HOME/POWER).


## 8. gettimeofday vs mach_absolute_time

The open question from hardware runs 10-13 (gltest: gettimeofday "0 ms",
then "1063 ms" while `mach_absolute_time` said 412 ms) is answered (re-log §34):

- `mach_absolute_time` is right; the timebase is 10^9 / 1047311.
- Userland `gettimeofday()` first tries the commpage. It needs ticks per
  microsecond, which is `1047311 / 10^6 = 1` here, and the kernel's
  reciprocal for a divisor of 1 overflows (M 0, add 1, shift 0, read back
  from the commpage in QEMU). Elapsed time then always reads 0: the result
  stays at the last syscall refresh for up to a second. Short intervals read
  0, long ones are off by up to a second, and whether the 2 s IORTC wait
  "helped" was just where a measurement fell.
- `commpage-gtod-off` stops publishing the timestamp, so userland always
  makes the exact syscall. Verified in plain QEMU (181 vs 182 ms) and with
  `--icount 2` (2254 vs 2254 ms, and over 154 s against the RTC).
- Still approximate: inside one second the kernel's own microtime path adds
  raw ticks as microseconds (up to 4.7 % fast until the next full
  conversion). A timebase that is a multiple of 1 MHz (e.g. PERIPHCLK/67 =
  2.0008 MHz) would remove both effects, but would change the clock code the
  whole system runs on; not done.
- On hardware, ios3ds-mcud logs `[mcud] clocks after ~60 s: RTC +N s,
  gettimeofday +… ms, mach_absolute_time +… ms`, and gltest's timing line
  should show equal values.

