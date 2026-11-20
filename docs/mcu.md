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
| Gyroscope | ITG-3270 class chip on I2C bus 3 (not the MCU) | Apple's AppleLIS331DLH driver on a new `gyro` DT node, two vtable words patched → IOHID gyro events → CoreMotion (`CMMotionManager` gyro / deviceMotion) | see §11 |
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
| 3 | ARM9 | accelerometer x/y/z (raw s16), sample count; gyro x/y/z (deg/s, 16.16, iOS axes), gyro sample count, `gyro_state`/`gyro_dev`/`gyro_id` (§11) |
| 4 | ARM11 | `req_seq`, `req_power` (power off), `req_acc` (accelerometer wanted), `backlight` (1 + level last set; 1 = screen off), `req_gyro` (gyro wanted) |
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
  two LCD backlights are also switched through MCU reg 0x22 (0x14 off,
  0x28 on), so the screens follow the shell even if iOS ignored the press.
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
`kernel/shim/audio.c`'s `shim_iic_transfer`). Details in re-log §40:
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

### 6.3 Lock = screen off (SBDontLockEver removed)

The rootfs used to set `SBDontLockEver` in com.apple.springboard.plist (an
early guard against the keybag/lock-state problem). With it set, SpringBoard
ignored the lock button and the screen stayed on (re-log §21).

Removing it alone was **not** enough. The first lock blanked the screen,
but after waking the picture stayed frozen on the old frame (status-bar clock
stuck). The fake CLCD page read `+0x00 = 2` (controller off) after the wake.
Cause: on display sleep the system lowers the CPU performance state, and
`AppleS5L8720XClockController` then sleeps on its command gate waiting for a
"change done" interrupt that the fake clock page never raises. It is the
same wait that hung every shutdown (§6.1). Patch `s5l-perf-nowait` marks the
change as done at once (re-log §41).

With both changes, in QEMU (`build/emu-l6`):
```
[input] key 30 down/up                                   (SELECT = lock)
[mcud] iOS brightness 0.50 x factor 0.00 -> LCD backlight 0/1000      (both screens black)
[input] key 40 down/up                                   (3DS HOME)
[mcud] iOS brightness 0.50 x factor 1.00 -> LCD backlight 500/1000
```
- After the wake the lock screen appears: "1:32, Wednesday, November 18",
  the charging battery, "slide to unlock".
- Sliding it unlocks back to the home screen, whose clock keeps running
  (1:33, 1:34). The CLCD reads `+0x00 = 3` again.
- Left alone on the lock screen, iOS turns the screen off again after about
  10 s, as on an iPod.
- Passcode-less unlock works; the stubbed keybag is not in the way.

Trade-offs:
- After a lock the user has to slide to unlock (touch works on the lock
  screen).
- With the screen off, iOS ignores touches; HOME, POWER, START or opening
  the shell wake it.
- If a hardware run shows trouble, put `SBDontLockEver` back
  (rootfs/recipe.json). The lock press is then ignored, and closing the
  shell still darkens the screens through MCU reg 0x22.

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
| Gyro: an ITG-3200-family chip at bus 3 / 0xD0 (Old 3DS, libn3ds "ITG-3270"), or an ST L3G at 0xD6, or something at 0xD2; the ARM9 can drive bus 3 (0x10148000) | libn3ds device table, Horizon's 14.375 LSB/(deg/s) | `[gyro] ITG-3200 family at I2C 3:d0 (WHO_AM_I 68), asleep, …` (good), `[gyro] no gyroscope on I2C bus 3` (then CoreMotion gets no rates); `gyro=0` in boot.cfg if bus 3 misbehaves |
| Gyro axes = console axes (x right, y to the hinge, z out of the lower screen) | guess | turn the console in a gyro app (or diag=1: `[gyrotest]` lines): a turn about the screen's vertical axis should show in iOS y. Fix with `gyro_axes=` (e.g. `x,-y,z`) |
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

## 9. Power saving while iOS' screen is off

"Off" = iOS has shown its display and AppleM2CLCD has switched the (fake) CLCD
controller off again: lock, auto-lock, or the shell closed (which locks).
`boot/arm9/powersave.c` and `shim_mcu_irq_hook` (tools/ios3ds-mcu/shim/mcu.c)
then:
- **pause the screen copy on both paths.** The ARM9 CPU copy already follows
  the CLCD enable bit (`display_active()`). The GPU mirror gets `want = 0` in
  its mailbox (`gm_want()`, gpu_mirror.h); the shim checks it before every
  frame. No GPU code changed.
- **stop reading the touch ADC.** Buttons, HOME/POWER and the shell are still
  read every loop.
- **let the ARM9 sleep.** Once md0 has been quiet for 200 ms, the monitor
  loop waits for an interrupt between iterations. ARM9 timer 1 wakes it every
  2 ms; IRQs stay masked in CPSR, so the pending IRQ only ends the wait. A
  block request is served within 2 ms, then the loop stays busy for the next
  200 ms.
- **mask the VBlank interrupt on the ARM11.** GIC 42 is masked at the
  distributor on the next VBlank. It is unmasked from the timer interrupt or
  a `kern.ios3ds_mcu` read as soon as the CLCD is on again. The CLCD frame
  IRQ is not injected while the controller is off anyway.
- **wake:** everything is restored, and the next loop copies a fresh frame
  (`last_blit` is reset).

The ARM9 logs both transitions with the rates. `powersave=0` in boot.cfg
turns all of it off; the rates are then still logged, for comparison.

**No system sleep.** Without SBDontLockEver, iOS put the whole system to
sleep about a minute after a lock ("System Sleep", "vic[0] ena 00010000"
in the log). Nothing on the 3DS can wake it: on an iPod the PMU interrupt
does. ios3ds-mcud therefore holds a `NoIdleSleepAssertion`
(`IOPMAssertionCreateWithName`; argument `sleepok` disables it). The screen
still goes off and the CPUs idle.

**Fewer wakeups while off.**
- **Input daemon:** `ios3ds-input` polls its mailbox at 10 Hz instead of
  60 Hz while the screen is off. It reads `screen_off` from
  `kern.ios3ds_mcu`; button samples queue in the 30-deep ring, so HOME and
  POWER still arrive within 100 ms. Its hold timing now counts real sleep
  time.
- **ios3ds-mcud:** runs at 4 Hz instead of 10 Hz while the screen is off,
  reading the brightness every loop so a wake turns the backlight on at
  once.
- **Kernel:** the boot-args now say `serial=1` instead of `serial=3`.
  Bit 1 started XNU's serial keyboard thread, which polled the
  (never-ready) UART every 16 ms, at 62.5 Hz with the screen on or off
  (re-log 48). Console output is unchanged.

**QEMU** (plain TCG; locked about 80 s; shim counters read with
`xp /24wx 0x20840040`; host CPU from the vCPU threads):

| | screen on | off, before (powersave only) | off, now |
|---|---|---|---|
| ARM9 monitor loops/s | 1837 | 520 (493 in WFI) | 530 (495 in WFI) |
| ARM9 vCPU thread, host CPU | 42 % | 2 % | 2 % |
| ARM11 IRQs/s | 127 (31 VBlank) | 245 (0 VBlank) | 146 (0 VBlank) |
| ARM11 timer IRQs/s | ~96 | 243 | 143 |
| ARM11 set_decrementer calls/s | 296 | — | 416 |
| ARM11 vCPU thread, host CPU | 7 % | 14 % | 10 % |

For comparison, powersave=0 while off: 4371 ARM9 loops/s, 304 ARM11 IRQs/s
(59 of them VBlank).

**Where the remaining ARM11 timer interrupts come from** (re-log 48):
- Method: breakpoints in QEMU's gdbstub on the shim's `set_decrementer`
  (backtraces including user frames) and on the `thread_call` worker's
  function call.
- What arms timers while the screen is off: `usleep` in ios3ds-mcud, in
  ios3ds-input and (1 Hz) in ios3ds-wifid; kernel `thread_call` timers,
  which are BSD networking callouts (resolved callees: `ip_output+0x86f4`,
  `net_del_domain+0x5c/+0xb8`, and others); and the serial keyboard poll,
  now gone.
- The CoreAnimation guess was wrong: no SpringBoard or QuartzCore frames
  show up among the timer arms.
- The networking callouts' share of the timer arms doubles when the screen
  goes off, with the same mix of functions. My guess: with the screen on,
  the busy (emulated) CPU makes them fire less often. They are left alone
  (they are XNU's own protocol timers); about 140 timer interrupts per
  second on an idle ARM11 cost well under 1 % CPU.

**"Where is my backlight?" ~3x/s after a wake:** AppleM2CLCD's
`safe_enable_backlight` (0x807a95d0) finds no backlight service, logs, and
keeps its pending flag (+0xea0). So it retries on every frame until the next
sleep. Patch `clcd-backlight-retry-off` clears the flag instead: after a
wake there are 0 such lines (`build/emu-p3`); one remains at the lock
(`disable_backlight`).


**Run 17 (button presses lost after many locks).** The ARM9 loop samples the buttons and the MCU, so anything that slows the loop delays or loses them. With the log console visible (screen mode "bottom" or "top") the console painting did that: 1-15 passes/s. Consoles are now painted a few rows per pass, and CPU screen copies poll the buttons as well (re-log 57).

## 10. Notification LED (opt-in)

`notify_led=1` in boot.cfg: at ≤ 10 % battery, not charging, the ARM9
writes a red blink pattern to MCU register 0x2D. Otherwise it writes all
zeros (off), and it switches the LED off before a power-off.
- **Pattern** (3dbrew "MCU Services" / libn3ds `McuInfoLedPattern`): step
  time 0x2F, smoothing 0x2F, loop delay 0, then 32 steps each of red (0xA0
  for 8 steps, 0 for 8), green (0) and blue (0).
- **Default is off:** the timing values are not verified on hardware.
- **QEMU model:** captures the 100-byte stream (`qom-get /machine mcu-led-rgb0`,
  `mcu-led-steps`). `build/emu-p3`: `mcu-battery 8` + charger off →
  `[mcu] notification LED blinking red: battery low`, steps 16, step-0 colour
  0xA00000; charger on → `[mcu] notification LED off`, steps 0.
- **Hardware check:** whether the LED blinks red at ~1 Hz and goes dark
  again. If the timing is wrong, adjust `p[0]`/`p[1]` in `led_update()`
  (boot/arm9/mcu.c).


## 11. Gyroscope → CoreMotion

iPhone 4 apps read the gyro through `CMMotionManager` (`gyroAvailable`,
`startGyroUpdates`, `gyroData.rotationRate`, `deviceMotion`). On 4.2.1 that
works in the app's own process: CoreMotion finds an IOHID service with usage
page 0xff00 / usage 9 in SpringBoard's HID event system, sets its
`ReportInterval`, and takes `IOHIDEvent` type 20 from it (x/y/z in deg/s; CoreMotion converts to rad/s).
RE details: [re-log.md](re-log.md) §64.

```
 gyro chip (I2C bus 3: ITG-3270 at 0xD0 on the Old 3DS, per libn3ds; 0xD6 / 0xD2 also probed)
   │  ARM9 (boot/arm9/gyro.c): asleep until iOS wants it, then 100 Hz;
   │  counts -> deg/s (16.16) -> iOS axes (boot.cfg gyro_axes)
   ▼
 mcubox line 3: gyro[3], gyro_samples, gyro_state        line 4: req_gyro (ARM11 -> ARM9)
   ▲                                                        ▲
   │ shim_gyro_dispatch (shim +0x2C)                        │ CTRL_REG1 power bits (shim_mcu_iic)
 Apple's AppleLIS331DLH on /arm-io/i2c0/gyro (AppleEmbeddedAccelerometer kext)
   │  vtable +0x3b4 getPrimaryUsage -> 9, +0x3c0 dispatch -> shim (kernel/patches-mcu.json)
   ▼
 IOHIDEventService (0xff00/9) -> SpringBoard's IOHIDEventSystem -> CoreMotion in the app
```

- **Kernel.** The n72 kernelcache has no gyro driver, but the accelerometer
  kext also contains `AppleLIS331DLH`, a class nothing on the n72 uses. A
  second I2C node, `/arm-io/i2c0/gyro` (`accelerometer,lis331dlh`, address
  0x6b, `tools/ios3ds-mcu/dt_mcu.py`), starts it. Two words of its vtable are
  patched: `getPrimaryUsage` returns 9 (gyro) instead of 3, and
  `dispatchAccelerometerEvent` goes to `shim_gyro_dispatch`, which calls
  IOHIDFamily's own `dispatchGyroEvent` (the next vtable slot) with the
  ARM9's newest sample. The driver itself is unchanged: it registers the HID
  service, runs its timer at the `ReportInterval` CoreMotion sets, and
  switches the "sensor" on and off through CTRL_REG1. The shim answers its
  I2C transfers (WHO_AM_I 0x32, BOOT cleared) and turns the power bits into
  `req_gyro`.
- **CoreMotion's gate.** `gyroAvailable` comes from a per-model table in
  CoreMotion (`CLPlatformInfo::getProviders`), not from IOKit: N72 says "no
  gyro". The rootfs recipe sets the gyro bit in N72's word (0x2012 → 0x2052,
  N81's value), so `gyroAvailable` and `deviceMotionAvailable` are YES.
- **Power.** The chip sleeps (ITG PWR_MGM SLEEP / ST power-down) from boot
  until a CoreMotion client starts updates, and again when the last one stops
  (the driver powers its "sensor" down when its ReportInterval is cancelled).
  While awake the ARM9 reads 7 bytes every 10 ms.
- **Units and axes.** ITG-3200 family at FS_SEL 3: 14.375 LSB per deg/s (the
  coefficient Horizon's HID reports for the Old 3DS); ST L3G at 2000 deg/s: 70
  mdeg/s per LSB. Converted to deg/s × 65536 (`mcu_gyro_fixed`,
  boot/common/mcu_proto.h). `gyro_axes=-y,x,z` (default) gives iOS x, y, z
  from the chip's x, y, z, assuming the chip's axes are the console's (x
  right, y towards the hinge, z out of the lower screen); with iOS shown in
  portrait on the lower screen, iOS y = console x and iOS x = -console y, as
  for the accelerometer (§5).
- **Kill switch:** `gyro=0` in boot.cfg. The ARM9 never touches bus 3 and the
  shim answers the driver's probe with kIOReturnNoDevice, so there is no gyro
  service. `gyroAvailable` still says YES (the CoreMotion word is in the
  rootfs), but no rotation rates arrive. `mcu=0` (or a dead MCU) has the same
  effect, since the gyro shares the MCU mailbox.
- **Test tool:** `/usr/libexec/ios3ds-gyrotest` (tools/ios3ds-mcu/gyrotest),
  a diag job (`diag=1`): uses `CMMotionManager` like an app for 150 s and
  logs `[gyrotest]` lines.
- **QEMU:** an ITG-3200 at bus 3 / 0xD0. QOM properties `gyro-x`, `gyro-y`,
  `gyro-z` (millidegrees per second about the console's axes, read/write),
  `gyro-awake` and `gyro-reads` (read only).

### 11.1 QEMU results (2026-11-20, `--icount 2`, diag=1)

`emu/run.py --icount 2 --time 560 --hmp "3=qom-set /machine gyro-x 10000;…"`
(`build/emu-g2`, not committed). Model rates in deg/s about the console's
axes: (10, 30, −45), then y −90 at 280 s, all 0 at 330 s, z 180 at 380 s.
Expected in iOS axes (default map −y,x,z): (−30, 10, −45) °/s =
(−0.524, 0.175, −0.785) rad/s, then x +1.571, then 0, then z 3.142.
```
[gyro] ITG-3200 family at I2C 3:d0 (WHO_AM_I 68), asleep, axes -y,x,z
[gyrotest] gyroAvailable 1 deviceMotionAvailable 1 accelerometerAvailable 1
ios3ds-mcu: gyro on
[gyro] on by iOS (0 samples so far)
ios3ds-mcu: gyro event #1: -1964940 656499 -2949690 deg/s/65536 (ARM9 sample 13)
[gyrotest] started (interval 20 ms): gyroActive 1 deviceMotionActive 1
[gyrotest] t=1 gyro -0.523 0.175 -0.786 rad/s | deviceMotion rate -0.518 0.175 -0.778 | attitude roll 1.02 pitch 1.25 yaw -0.93
[gyrotest] t=90 gyro 1.571 0.175 -0.786 rad/s | deviceMotion rate 1.916 0.030 0.031 | attitude roll -2.55 pitch -0.66 yaw 2.15
[gyrotest] t=100 gyro 0.000 0.000 0.000 rad/s | deviceMotion rate 0.339 -0.165 0.769 | …
[gyrotest] t=130 gyro 0.000 0.000 3.142 rad/s | deviceMotion rate 0.299 -0.157 3.753 | attitude roll 2.28 pitch -0.20 yaw 1.82
ios3ds-mcu: gyro off
[gyrotest] done: gyro value changed 6 times, 1305 non-zero reads, last 0.000 0.000 3.142; updates stopped
[gyro] off by iOS (14782 samples so far)
```
- `gyroData.rotationRate` equals the model's rates in iOS axes to 3
  decimals. `deviceMotion` runs CoreMotion's sensor fusion on top: its
  rotation rate follows with its own bias estimation (it converges to the
  gyro while the accelerometer is still; steps show the filter catching up),
  and the attitude turns.
- Power: the chip is asleep from boot until the first client and again after
  `stopGyroUpdates` (`qom-get /machine gyro-awake`: false at the end; the
  ARM9 sampled 14782 times, ~100 Hz while on).
- The first run sent rad/s and CoreMotion reported 1/57.3 of the rates: its
  HID unit is deg/s (re-log §64). Without `gyro-interrupt-calibration` it
  also logged `CLDeviceMotion::startSensorFusion … Could not get gyro dt`;
  with the property that line is gone.
