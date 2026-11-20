# Handoff: 3DS cameras in iOS (state 2026-11-20)

**Status in one line:** the cameras worked only in QEMU until build `m1-0.3.0-43-g53304df`. On that build's first hardware boot the camera self-test printed **`<-- DATA`** (reported by the user from the screen; the log, `run29`, is not read yet). So a CAM block received sensor data on real hardware for the first time.

## 1. Background

| Piece | Where | Docs |
|---|---|---|
| iOS side: Celestial's virtual-camera plug-in (Camera.app, AVFoundation) | `tools/ios3ds-camera/vcd/vcd.c` | `docs/camera.md` §2 |
| Sensor bring-up over I2C, CAM block reset/init, boot self-test | `boot/arm9/camera.c` | `docs/camera.md` §3, §3.1, §7 |
| FIFO drain (ARM11-only data window, every 1 ms while a camera runs) | `kernel/shim/cam.c` | `docs/camera.md` §4 |
| QEMU model (3 sensors, CAM blocks, moving test pattern) | `emu/qemu/hw/arm/ctr_cam.c` | `docs/emulator.md` |
| Reverse-engineering log | `docs/re-log.md` §65 (design), §68 (run 23), §70 (run 25), §74 (Nintendo's CAMERA module) | |

Sensor/port map: outer left camera → CAM1, outer right and inner cameras → CAM0. All three sensors are Aptina MT9V113 (chip ID 2280). Back camera in iOS = outer left, front = inner.

## 2. Hardware history

| Run | Build | Result |
|---|---|---|
| 23 | `m1-0.3.0-15` | all 3 sensors answer on I2C, outer left "streaming", **0 frames, 0 overruns** |
| 25 | `m1-0.3.0-27` | sensor runs (`SEQ_STATE 3`, parallel output on); CAM blocks never toggle a busy bit or fill the FIFO, on any variant; `CFG11_CAMERA_CNT` bit 1 doesn't stick; CAM1 bit 4 doesn't stick |
| 27 | `m1-0.3.0-33` | boot self-test (3 sensors × 8 set-ups × 2 blocks): `0 set-ups saw data` |
| 29 | `m1-0.3.0-43` | **`<-- DATA` on screen** (user). Log not read yet |

Logs: `docs/hwlogs/run23.txt`, `run25.txt`, `run27.txt`; `run28.txt` is the card's log from just before build 43 was copied. `run29` will be `build/hwlogs/run29.txt` once the card is read: `scp devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/log.txt`.

## 3. What changed in build 43

Nintendo's CAMERA sysmodule (title `0004013000001602`) was reverse engineered from the user's own copy. `docs/camera.md` §3.1 and re-log §74 have the derived sequence in our own words. Four likely bugs in our driver were fixed:

1. **Reset/clock order:** CAM0 `CAM_CNT` bit 4 is the reset for *both* blocks. It must be cleared, the camera clock switched on, then bit 4 set again while the clock runs. Before, we turned the clock on with bit 4 already set.
2. **Bit 14 was missing on the other block.**
3. **Stopping:** the shim stopped a block by writing `CAM_CNT = 0`. On CAM0 that clears bit 4 and holds both blocks in reset. Now stop clears only bit 15.
4. **Transfer unit:** it is now a whole number of lines (lines × width × 2 bytes; variant 0 = 2560 bytes = 4 lines of 320).

`pdn:c` is just camera power/clock on/off; on the ARM9 that maps to `CFG11_CAMERA_CNT` bit 0. `mcu::CAM` is only the camera LED.

## 4. Reading the next log (run 29)

**Before XNU** (`diag=1` only; the boot self-test):
- `[cam-test] <sensor>, cnt XXXX +6 YY: CAM0 cnt … stat … max … | CAM1 …`: one line per try. **`<-- DATA`** marks the working sensor / block / set-up.
- `[cam-test] done: N set-ups saw data`.

**At camera power-on:**
- `[cam-diag] after the cam module's block reset/init`. Expected `CAM0 cnt 00004610`, `CAM1 cnt 00004600` (that is what QEMU shows).

**While Camera.app runs, every 10 s:**
- `[cam] outer left camera: N frames, M FIFO overruns …; shim: S services, D with data, stat …, CAM_CNT …, variant V, CAMn`
- Frames > 0 means the runtime path works too. `still probing` means the driver hasn't found a working set-up yet.

## 5. Next steps

1. **Read run 29.**
   - Which sensor, block and set-up got data in the self-test?
   - Does the runtime driver (Camera.app) also get frames? It can differ: the self-test runs on the ARM9 before XNU and only reads `CAM_STAT`, while the runtime path is the shim's ARM11 FIFO drain.
2. **If only the self-test sees data:** compare the two paths: register order, which block is enabled, transfer unit, bit 15 timing. The self-test's working set-up becomes runtime variant 0.
3. **Once frames arrive:**
   - Check colour byte order (YUV422 → BGRA), orientation and mirroring, and the front/back switch. Camera.app only shows its switch button once a preview is running.
   - Check FIFO overruns. XNU masks interrupts during disk reads, so the 1 ms CPU drain may lose data; a DMA-based drain (CDMA) would fix that.
   - Real VGA stills: today stills are interpolated from 320×240.
4. **Clean up:** keep the self-test behind `diag=1`, drop the variants that never work, and update `docs/camera.md` §7 and the README table (Cameras ❌ → ✅).
5. **If nothing works at runtime at all:** reverse the `pdn` sysmodule. `pdn:c` may set more than `CFG11_CAMERA_CNT` bit 0.

## 6. Nintendo's code: rules

- **The user's CAMERA module copy:**
  - private, outside all repositories: `~/.local/share/ios3ds-private/cam.code` (BLZ-decompressed, 98,304 bytes; `cam.code.raw` = as copied, 63,460 bytes);
  - also on the SD card as `sd:/ios3ds/cam.code`;
  - **never commit it or quote large parts of it.** Only derived facts in our own words go into the repo.
- **Disassembly:** load base 0x00100000, mostly Thumb (`arm-none-eabi-objdump -D -b binary -marm -M force-thumb --adjust-vma=0x100000`). Horizon maps physical I/O 0x10100000+ at virtual 0x1EC00000+, so CAM0 is 0x1EC20000 and CAM1 0x1EC21000.
- **Getting a copy:** the `.3dsx` launcher copies `cam.code` like `nwm.code`. Path `678A/2` (archive 0x2345678A, file path `{0, 0, 2, ".code"}`) works through plain FS on the user's Old 3DS. GodMode9 fallback: `tools/ios3ds-camera/ios3ds-cam-dump.gm9`.
