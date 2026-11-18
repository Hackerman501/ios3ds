# Handoff (2026-11-18)

Where ios3ds stands and what to do next. Read [README.md](README.md) and [docs/architecture.md](docs/architecture.md) first; [docs/re-log.md](docs/re-log.md) has every reverse-engineering step.

## 1. Status (main `ee9dec0`, on the SD card as `m1-ee9dec0`)

The user is away. Everything after hardware run 13 is verified in QEMU only. The QEMU GPU model is now as strict as run 13's hardware. The next hardware run should read the log lines listed in docs/pica200.md §11–14, docs/mcu.md §7 and docs/audio.md.

| Area | QEMU | Real Old 3DS |
|---|---|---|
| Boot to SpringBoard, touch, buttons, apps | works; soak 5: 40 cycles PASS | works (runs 8–13, ~66–82 s to input) |
| md0 | waits with IRQs on under a kernel mutex (re-log 31) | old IRQs-off version tested |
| Screen copy on the PICA200 (512x512 texture, clock setup) | ACTIVE, pixel-identical; one plain-QEMU timeout open (re-log 39) | run13: failed on the non-power-of-two texture, so CPU copy; the fix is not yet run |
| ARM9 CPU screen copy (fallback) | bursts, displayed set only | 300–430 ms/frame (run 12/13); the faster version is not yet run |
| GL on the PICA200 | gltest PASS (clears drawn, no FCRAM fills) | not yet run |
| Audio output (I2S start patch, CSND) | bit-exact PCM, volume keys | not yet run (run 8–10: "could not start DMA") |
| MCU: RTC (shown as the 3DS's local time), battery, HOME/POWER/shell, volume slider, brightness, shutdown | works ([docs/mcu.md](docs/mcu.md)) | not yet run |
| `gettimeofday` | fixed (`commpage-gtod-off`, re-log 34) | run12/13 showed the bug |
| Accelerometer | in progress (agent/mcu: Apple's LIS331DLH driver path) | — |
| Microphone | in progress (agent/audio2) | — |
| CoreAnimation compositing on the GPU | in progress (agent/gpu) | — |
| Wi-Fi | off cleanly without `wifi.cfg` | not tested |

## 2. How work is organised now
- **Release tree:** clean worktree `~/Documents/GitHub/ios3ds-wt/rel` (branch `rel-main`, pushed to `main`). It has its own unified QEMU build (GPU + CSND + MCU models). Card builds come only from there.
- **Agents** in their own worktrees: `ios3ds-wt/pica` (agent/gpu), `ios3ds-wt/mcu` (agent/mcu), `ios3ds-wt/audio2` (agent/audio2). Merge order and re-log numbering are resolved at merge time.
- **Stability:** `python3 emu/soak.py --cycles 40` (re-log 31, 39). It boots, opens every icon, presses Home, swipes, locks and unlocks, then scans the log. Soaks 2–5 found no panics, stalls, I/O errors or crashes.
- **Disk:** the machine is at ~97%. Remove worktrees' `build/sd.img` / `build/emu` when not testing.

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
6. RTC: done on `agent/mcu` (PEGetGMTTimeOfDay from the MCU RTC, no IORTC wait); hardware check pending. Accelerometer → SpringBoard needs a kernel IOHID service (docs/mcu.md §6.2).
7. A fast swipe right didn't open Spotlight in QEMU (possibly test timing).
