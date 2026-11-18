# Handoff (2026-11-17, late evening)

Where ios3ds stands and what to do next. Read [README.md](README.md) and [docs/architecture.md](docs/architecture.md) first; [docs/re-log.md](docs/re-log.md) has every reverse-engineering step.

## 1. Status

| Area | QEMU board | Real Old 3DS |
|---|---|---|
| Monitor, SD, ARM11 bring-up, kernel load | works | works |
| XNU boot to SpringBoard | works (also with `--icount 2`) | **works**: runs 8–11. SpringBoard + input before ~66–82 s (run8: ~320 s) |
| Buttons (Home = START, lock = SELECT, volume = L/R) | works | works (run10/11: `key 40`, `key 30`, `key ea`) |
| Touch (GS hand events, `flipx`) | works | works: Settings, Notes, Mail, App Store opened |
| Swipes (paging/bounce end after lift) | fixed (re-log §29) | fix not yet run on hardware |
| Screen modes (X: top / both / bottom) | works | works |
| GL ES 1.1 software engine (`tools/ios3ds-gl`), gltest | PASS | PASS, 1089 ms per offscreen frame (run9) |
| PICA200 GL back end + GPU display mirror | PASS, mirror pixel-identical to the CPU copy | **not yet run on hardware** (re-log §27–28) |
| Wi-Fi (ARM9 AR6014 + `ios3ds-wifid`) | off cleanly without `wifi.cfg` | not tested |
| Audio (shim PL080/I2S/I2C emulation → CSND) | kexts start | CODEC answers; `AppleEmbeddedAudioDevice: could not start DMA: device is not ready` |
| `.3dsx` launcher (rename swap + restore) | — | works |
| MCU ([docs/mcu.md](docs/mcu.md)): RTC/calendar, battery icon, HOME/POWER, shell, volume slider, brightness, shutdown → power off | works (branch `agent/mcu`) | not tested; §7 of mcu.md lists the log lines |
| MCU accelerometer (Apple's LIS302DL driver on an emulated sensor) | works: Safari rotates with `mcu-orient` (docs/mcu.md §6.2) | not tested; kill switch `mcu_acc=0` |
| Lock = screen off (SBDontLockEver removed, `s5l-perf-nowait`) | works: lock → backlight off, HOME → lock screen → slide to unlock (docs/mcu.md §6.3) | not tested |

## 2. Current work

**Hardware history:** docs/hwlogs/README.md (run1–run11) and re-log §25–26.
- **Old hang:** the clock base was 0. Fixed by the lazy shim init.
- **Speed-ups:** chunked logging, md0 served during screen copies and with a 2 ms linger, and the 30 s IORTC wait cut to 2 s.

**Solved (agent/mcu, re-log §31): `gettimeofday` early in boot.** The commpage fast path froze it for up to 1 s (usec divisor 1 → degenerate reciprocal); patch `commpage-gtod-off`. The notes below are the history.

**Was open: `gettimeofday` early in boot.**
- gltest's offscreen frame reads `0 ms` on hardware in run10 (IORTC wait 0 s) and run11 (2 s); run9 (30 s) read 1089 ms.
- The 2 s wait fixed it in QEMU, but not on the 3DS.
- The next build's gltest logs `gltest: timing: gettimeofday rc … errno … tv … -> …; mach_absolute_time … ms`. That line settles whether the call fails or the clock is frozen.
- `wall_us()` in the input daemon's touch pacing also uses `gettimeofday`.

**Next hardware run (build from `ios3ds-wt/rel`, branch state = main after this push):**
- **Includes:** the GPU mirror and the PICA GL back end (agent/gpu), the swipe fix (fix/swipe), and the gltest timing line.
- **The rootfs changed** (MBXGLEngine, gltest), so copy `rootfs.hfs` too.
- **GPU:** read the log lines listed in docs/pica200.md §11.
- **Kill switches in `sd:/ios3ds/boot.cfg`:** `gpu_mirror=0` (the ARM9 copies the screen again) or `gpu=0` (the kernel never touches the GPU).

**Build hygiene:** card builds come from the clean worktree `~/Documents/GitHub/ios3ds-wt/rel` (its `build/` symlinks staging, toolchain and qemu from the main checkout). Agents' uncommitted edits in the main checkout once leaked into a test build and broke it (re-log §26). The userland helpers are built separately: `make -C tools/ios3ds-{sample,gl} tools/ios3ds-input/daemon tools/ios3ds-wifi/daemon`, then `make sdimg`.

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
- **On the 3DS:** launch from the Homebrew Launcher (`ios3ds.3dsx`), or Luma chainloader → `ios3ds.firm`. `log.txt` gets `[hb]` heartbeat lines (every 10 s while the console is quiet) and `[probe]` lines after a stall.
- The reader PC sometimes drops off the network or needs the card re-mounted (polkit blocks `udisksctl` over SSH). If `/sys/block/mmcblk0/ro` is 1, the card's lock switch is on.
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
| `main` | everything merged: GL software engine, Wi-Fi, audio, input, screen modes, launcher, HW probes and heartbeat, ARM9 performance, PICA200 GL back end and GPU display mirror (`agent/gpu`), swipe fix (`fix/swipe`) |
| `agent/pica`, `agent/gpu`, `fix/swipe` | merged; kept for history |

## 6. Backlog

1. Hardware run of the GPU build: mirror self-test variant, fps, `[gl] pica: GPU up`, PICA vs software frame time (docs/pica200.md §11).
2. `gettimeofday` early in boot (§2).
3. Audio on hardware: `could not start DMA: device is not ready` ([docs/audio.md](docs/audio.md)).
4. Wi-Fi on hardware: `sd:/ios3ds/wifi.cfg` (SSID/PSK) and the NWM firmware extraction ([docs/wifi.md](docs/wifi.md)).
5. CoreAnimation's GL compositor on the PICA engine (docs/gpu.md, open item).
6. RTC: done on `agent/mcu` (PEGetGMTTimeOfDay from the MCU RTC, no IORTC wait); hardware check pending. Accelerometer done the same way (Apple's LIS302DL driver, docs/mcu.md §6.2).
7. A fast swipe right didn't open Spotlight in QEMU (possibly test timing).
