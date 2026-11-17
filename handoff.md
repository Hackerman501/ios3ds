# Handoff (2026-11-17, updated in the evening)

Where ios3ds stands and what to do next. Read [README.md](README.md) and [docs/architecture.md](docs/architecture.md) first; [docs/re-log.md](docs/re-log.md) has every reverse-engineering step.

## 1. Status

| Area | QEMU board | Real Old 3DS |
|---|---|---|
| Monitor, SD, ARM11 bring-up, kernel load | works | works |
| XNU early boot | to SpringBoard home screen (also with `--icount 2`) | run7 (`3c57ea7`): past the old hang to `launchd[1] has started up`, then quiet with the kernel alive; run8 (`9867d28`) adds the `[hb]` heartbeat to tell slow from stuck |
| Buttons (Home, lock, volume via GraphicsServices) | works | not reached |
| Touch (GS hand events, `flipx`) | works | not reached |
| Screen modes (X: top / both / bottom) | works | works in the monitor |
| GL ES 1.1 software engine (`tools/ios3ds-gl`), gltest | PASS | not reached |
| Wi-Fi (ARM9 AR6014 + `ios3ds-wifid`) | off cleanly without `wifi.cfg` | not reached |
| Audio (shim PL080/I2S/I2C emulation → CSND) | kexts start | CODEC answers (id 11 rev 16) |
| `.3dsx` launcher (rename swap + restore) | — | works (`launcher swap: restored`) |

## 2. The hardware hang after "iBoot version": fixed, needs a hardware run

**Cause** (details in [docs/re-log.md §25](docs/re-log.md)):
- `rtclock_init` stores `mach_absolute_time()` as the tick base (`rtclock+0x30`).
- It runs before the first `set_decrementer`, and the shim used to initialise only there. Until then `shim_get_abstime` returned 0, so the base was 0.
- The calendar conversion at `0x8006916c` then spins, with IRQs masked, until the first tick sets the base. That tick cannot arrive with IRQs masked.
- Plain QEMU hid it: its wall-clock timer fired before the spin.

**Fix:** `shim_get_abstime` now initialises the shim on the first read.

**Verified in QEMU:**
- `python3 emu/run.py --icount 2` (instruction-clocked time, ~250 MHz) reproduced the exact hardware probe values before the fix: `abs 34 cpsr 93 | dec 1 l10473 @800695d3`.
- With the fix, it boots through launchd to SpringBoard.
- The plain-mode boot still reaches the home screen.

**Hardware run 7** (`docs/hwlogs/run7.txt`): got past the hang and reached `launchd[1] has started up`. The log ends there with no stall dump. QEMU next prints launchctl's messages and `Running fsck on the boot volume...`.

**Run 8 build `9867d28` is on the card.** While the console is quiet, it logs `[hb]` lines every 10 s:
- counters and md0 requests/errors;
- the last 8 IRQ-time PC samples (`u`/`k` = user/kernel mode, `t` = Thumb).

These tell slow from stuck, and where the ARM11 is. In QEMU with `--icount 2`:
- md0 serves only ~8 requests/s;
- launchd starts at t≈110 s.

Known 30 s delay on every boot, QEMU included: `AppleARMPE::getGMTTimeOfDay` still waits for the RTC. The `rtc-wait-0s` patch zeroes the timeout passed to `waitForService` at 0x802e78c6, but the wait still happens.

**Next step: run build 9867d28 on the 3DS and leave it running for at least 10 minutes.**
- If it stops again, `log.txt` now ends with `[probe]` lines (written after ~5 s without a clock read or console output): shim counters, timer/GIC state, MMU registers, and the vector page as the ARM11 sees it vs. FCRAM.
- Copy the log back as in §3. No photo needed.
- Reproduce any new hardware-only behaviour with `--icount 2` first, then use `emu/gdb/postmortem.py` (attach to a running `-s` QEMU) to see where the ARM11 is.

## 3. Hardware workflow

- **SD card reader:** `devilapi@192.168.178.83` (SSH key auth); the card mounts at `/run/media/devilapi/3ds`.
- **Before copying a new build**, save the old log:

  ```
  scp devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/log.txt build/hwlogs/runN.txt
  ```
- **Copy the build.** `rootfs.hfs` (1 GB) is already on the card; only re-copy it when the rootfs changed:

  ```
  rsync -rt --modify-window=1 --no-perms --exclude rootfs.hfs build/sd/ devilapi@192.168.178.83:/run/media/devilapi/3ds/
  ssh devilapi@192.168.178.83 sync
  ```
- **On the 3DS:** launch from the Homebrew Launcher (`ios3ds.3dsx`), or Luma chainloader → `ios3ds.firm`. Probe values appear on the bottom screen, rows 20–29.
- **Safety rules:**
  - nothing ever writes NAND or FIRM;
  - Apple files are never committed (everything is generated from the IPSW at build time);
  - every build shows its version string on screen.

  See [docs/real-hardware.md](docs/real-hardware.md).

## 4. Building on a new machine

- Put the iPod2,1 4.2.1 IPSW where the README says, then run `make sd`. That builds the firm, kernel, shim, rootfs and `build/sd/`. `make test` runs the unit tests.
- **QEMU:** the patched tree is `emu/qemu`; it builds to `build/qemu/b/qemu-system-arm`. See [docs/emulator.md](docs/emulator.md).
- **Viewer:** `emu/viewer.py` (Tk + QMP). Headless touch test: `emu/tap.py`.
- **3DS launcher:** needs devkitARM + libctru (here taken from `../rockbox-3ds/scripts/env.sh`). Then run `make -C tools/ios3ds-launcher`.

## 5. Branches

| Branch | Status |
|---|---|
| `main` | everything merged (GL software engine, Wi-Fi, audio, input, screen modes, launcher, HW probes) |
| `agent/pica` | superseded by `agent/gpu` |
| `agent/gpu` | `agent/pica` + main, reviewed for real hardware, plus the GPU display mirror (the screen copy moves from the ARM9 to the PICA200, self-tested, ARM9 copy as fallback). New: `gpu.bin` (shim GPU part + command lists, `load=` in boot.cfg), mailbox at shim+0x7F00, boot.cfg `gpu=` / `gpu_mirror=`. See docs/pica200.md §9-11. |

## 6. Backlog after the hardware boot works

1. Real hardware, stages 2–4 in [docs/real-hardware.md](docs/real-hardware.md): root disk over real SD timing, SpringBoard, input.
2. Merge `agent/gpu`, then test PICA200 on hardware (docs/pica200.md §11 lists the log lines to read).
3. Wi-Fi on hardware: `sd:/ios3ds/wifi.cfg` (SSID/PSK) and the NWM firmware extraction ([docs/wifi.md](docs/wifi.md)).
4. Audio on hardware: speaker and headphones through CSND ([docs/audio.md](docs/audio.md)).
