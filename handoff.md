# Handoff: 3DS cameras in iOS (state 2026-11-20, after run 29 / build 44)

**Status in one line:** the cameras work end to end on hardware as far as the
counters go (build 43, run 29: ~12 fps of real sensor data into Camera.app,
0 FIFO overruns), but Camera.app's preview shows one "first frame" whose
content wiggles around. Build 44 adds frame dumps + two independent
starvation watchers to decide where the wobble is born.

## 1. Run 29 (`build/hwlogs/run29.txt`) — what it proved

- **Boot self-test (diag=1): all 36 set-ups `<-- DATA`.** Each sensor streams
  on its mapped block (outer left → CAM1, inner → CAM0).
- **Runtime: every session streams.** ~12 frames/s (≈450 FIFO words/ms;
  words = frames × 38400 exactly, all boot long), 0 FIFO overruns, 0 CAM
  restarts. The plug-in delivers into Celestial (per-session "first frame
  delivered", e.g. 566 frames in the long session) and Celestial dequeues
  everything (0 queue-full drops).
- **The ARTIFACT:** the user sees one picture, looking like the first
  captured frame, wiggling around. Sessions: 7 camera sessions across two
  mediaserverd processes (device re-created 2× in the first, fast front/back
  switches in between); the last was short (~2 s, 14 frames) before the user
  pressed Home.
- **Smoking gun:** the 5-s diag after one stop saw the CAM1 FIFO **parked at
  the fill cap (1280 units) with CAM_STAT bit 15 still clear** — a full FIFO
  does not raise the overrun bit on this hardware. So drain starvation
  (XNU's masked interrupts; the GPU mirror alone showed 29 ms IRQ latency
  in this boot) silently discards words, which displaces the 38400-word
  frame window per incident — a picture that wanders exactly like "wiggling".
  Alternatively the wobble is display-side; nothing in the counters can tell
  the two apart.
- 15 fps pixel rate would be ~576 words/ms; the observed ~450 words/ms is
  either the sensor's real rate or a starved one — unknown until build 44's
  data.

Numbers cheat-sheet: the shim's `frames/words/services` are global-since-boot;
`<-- DATA` lines at log offsets 74-107 (self-test), sessions around clean-file
lines 721-765 (`device created` … `stopped after`), 767-803, 884-1046,
1048-1232, 1392-1427; key 40 = Home.

## 2. Build 44 (this tree): how it decides

Three new observers, no behaviour changes to the capture path:
1. **`drain gap max X ms`** in the 10 s `[cam]` line: the shim's worst
   interval between drain services (> ~5.5 ms = the FIFO's worth of stream →
   words lost, overruns or not).
2. **`[cam] monitor 10 s` / `at stop`**: the ARM9 samples `CAM_STAT`
   itself (it cannot be starved): max FIFO fill + near-full counts.
3. **Frame dumps:** at every camera stop the ARM9 copies the shim's **two
   newest complete YUV422 frames** out of its buffers (mailbox lines:
   `buf_pa[3]`, `newest(+seq)`, `snew(+seq)`, `gap_max`; the shim cleans the
   buffers in op_stop) to `sd:/ios3ds/CAM<seq>.RAW` (~300 KiB, one per
   session; log line `[cam] dumped frames seq …`). Layout:
   u32 × 8 header `0x44554D43 ('CAMD'), 1, 320, 240, cam, port, seq1, seq2`,
   then frame 1 (newest), frame 2. Analyse on the PC:
   `python3 tools/ios3ds-camera/analyze_dump.py CAM00001234.RAW`
   → BMPs of both frames in both YUYV/UYVY byte orders (settles the colour
   question by eye) + mean luma diff + best (dx,dy) frame alignment + rows
   whose best dx disagrees (mid-frame word loss). See `docs/camera.md` §4.1.
   The plug-in now logs `stopped after N frames this run (M total, …)`.

How to read the next run's log:
- `drain gap max` > 5 ms and/or `near-full` > 0 in monitor lines **and** the
  dump's frames drift (dx/dy ≠ 0 between neighbouring frames, shear rows) →
  words are lost behind masked interrupts → the fix is a **CDMA drain**
  (Nintendo's own driver uses CDMA, docs/camera.md §2/§6).
- Gap max small, monitor quiet, but the dumped frames **drift anyway** → the
  true sensor frame is not exactly 38400 words (blanking data reaching the
  FIFO): fix on the CAM block set-up (+6/trimming/SOFS-EOFS).
- Dumped frames stable and correct, diff ~ 0 across seconds → the sensors
  deliver one frozen scene: look sensor-side (SEQ/AE), and at the display
  path (pica CPU read-back fallback was active during run 29's last
  session).

## 3. Already validated here
- `make firm`/`shim`/`test`/`userland` green (host camera drain test included,
  340 checks OK); `python3 tools/ios3ds-camera/analyze_dump.py` self-tested
  (synthetic dump: detects dx=+4/dy=+1 and a sheared row exactly).
- QEMU end-to-end run of the dump path: see §5 (in progress at the time of
  this handoff — check `build/emu-camtest/`).

## 4. Deploy for the next hardware run (build 44)
The card was in the reader at handover time; deploy with:

    make sd CYDIA=0 TLSFIX=0      # or with Cydia if wanted
    scp -r build/sd/ios3ds/* devilapi@192.168.178.83:/run/media/devilapi/3ds/ios3ds/

(If the card is mounted elsewhere, adapt. `boot.cfg` camera=1 as before;
`diag=1` not needed — the dump needs XNU up.)

Run 30 procedure: boot, open Camera.app, hold ~20-30 s on the back camera,
switch to the front for ~20 s, close. Pull `sd:/ios3ds/log.txt` →
`build/hwlogs/run30.txt` **and** every `ios3ds/CAM*.RAW` → `build/hwlogs/`.
Then: read §6 (run numbers), run analyze_dump.py on each RAW.

## 5. QEMU validation of the dump path (do this if §3's item is not done)
`python3 emu/tap.py --icount 2 --time <T> --tap <t>:<camera icon x,y>
--key <t2>:h --shots ... --out build/emu-camtest/...` — Camera.app closes with
Home (graceful stream stop → dump). The log must show
`[cam] dumped frames seq …` and the SD image a `CAM00000???.RAW`; then
`python3 tools/ios3ds-camera/analyze_dump.py` on it (extract the file from
`build/sd.img`'s FAT, e.g. mtools/`emu/hfsmap.py`-style reader). A finding of
dx/dy = 0 and moving test pattern content = the dump path works and the
hardware numbers gain meaning.

## 6. Track of the old open points (unchanged)
- Colour byte order (Y0 U Y1 V vs UYVY), orientation/mirroring: decided by
  eye from the dump BMPs this coming run.
- Still image path: run 29 saw "0 stills" — no shutter tap reached vcd's
  `StillImageCaptureNow`; take one photo in run 30 to exercise the review
  image path on hardware (QEMU: photos save fine).
- VGA stills, front preview mirroring, video recording, outer-right camera:
  unchanged (docs/camera.md §8).
- `docs/camera.md` §7 documents runs 23/25/29 and the new diagnostics; README
  now says ⚠️ Cameras with the run-29 wording.

## 7. Nintendo code rules (unchanged, see previous handoff)
`~/.local/share/ios3ds-private/cam.code` stays private; only derived facts in
our own words go into the repo; disassembly base 0x00100000, Thumb,
CAM0 0x1EC20000 / CAM1 0x1EC21000 virtual.
