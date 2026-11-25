# Old 3DS frame drops: folder open and Safari load

Where the time goes when the user opens a folder on the home screen or loads
a page in Safari on an Old 3DS (128 MiB, 268 MHz ARM11), what was changed,
and how to measure it again. Hardware numbers come from the two perf logs
(`issue-reports/run2-perf.txt`: `stereo3d=0`, GPU mirror on; `run3-perf.txt`:
`stereo3d=1`). QEMU numbers are `--icount 2` (instruction-clocked guest time,
Old 3DS model) from `emu/perf.py` (this branch). Reverse-engineering steps
are in [re-log.md](re-log.md) 113.

## 1. What the hardware logs say

### 1.1 The display copy is the first bottleneck

| line (run2) | number |
|---|---|
| `[monitor] CPU screen copy: 25 copies in 30 s, 347.2 ms each (ARM9)` | the ARM9 copy of the 320x480 framebuffer to the LCDs took **347 ms/frame** before the GPU mirror's self-test passed (~3 fps) |
| `[gpu] mirror: 600 frames in 33 s (18.1 fps) … upload 9388 us, draw 3086 us, read-out 6475 us; frame avg 20661 us … 764 busy skips` | once the GPU mirror is active the display runs at **18 fps**, ~19 ms of GPU per frame, and **764 busy skips** in 33 s — the GL (CoreAnimation) compositor is holding the GPU |
| `[monitor] CPU screen copy: 11 copies in 33 s, 75.6 ms each (ARM9)` | the ARM9 still falls back to a 75 ms copy when the GPU is briefly busy |

So on the Old 3DS the screen copy alone used to be a 347 ms ARM9 stall, and
the GPU mirror that replaced it is still only 18 fps with the GPU split
between the mirror and CoreAnimation. Every animation the user sees is paced
by that ~20 ms/frame mirror.

### 1.2 Memory pressure grows through the run

`[mem N]` lines (run2): `free` goes 99.3 MiB → 52.3 MiB → **26 MiB**, `wired`
17.9 → 33.3 MiB (the 12 MiB GPU arena plus the kernel), `memorystatus_level`
85 → 60. SpringBoard peaks at 16.9 MiB resident. Free memory is not the
*folder* bottleneck (see §2), but it is what limits Safari on big pages
(re-log 104).

### 1.3 One scattered CA surface

`[pica] present: destination 0x04f52000 (+614400) not usable: not physically
contiguous` — CoreAnimation's second 320x480 surface (after an app returns to
the home screen) is in scattered FCRAM pages; the engine keeps the GPU
present via the arena copy (`PICA_OP_SCATTER`, re-log 78/79) instead of the
process-wide CPU read-back. Only that surface takes the detour.

## 2. QEMU: where the folder open goes

`emu/perf.py` taps the Utilities folder (iOS point 120,237, re-log 77) and
measures the wall time until the screen settles open, then Home and the time
back. On the Old 3DS model with `--icount 2` (host load makes the wall clock
noisy; the `[perf]` frame histogram below is guest time and is the robust
number):

| metric | value |
|---|---|
| boot to SpringBoard + input | ~70 s |
| folder open (3 runs) | 12–16 s wall, the first open slowest (host load makes this noisy) |
| folder close | 12–14 s wall |
| Safari cold launch | 10–12 s wall |

The robust numbers are the `[perf]` frame histograms below (guest time,
immune to host load): a folder open runs ~40 CA frames in ~1 s of guest time
at an average **24–26 ms/frame** (~40 fps), with a couple of frames dropping
to 66 ms and one to a few hundred ms. The `[gl] CA:` line for those windows
puts the render thread at **14–24 ms/frame** of CPU (icount 2), of which the
folder's first frames are 2–3 texture uploads/frame (the folder linen and
icons) settling to ~1.2/frame; GPU presents stay at 1.2–1.3/frame and CPU
read-backs at ~0.2/frame (the one-time present check), so the compositor is
not falling back to the CPU.

To see *why*, a run with `diag=1 ioprof=1 log_ts=1` (the folder opened at
~guest 30 s):

- **md0 is quiet:** `[io] t=26.802 … rd 96 KiB … arm9 3 ms` — the ARM9 serves
  a few MiB/s at 3–15 ms/s by the time the folder opens. The folder open is
  **not** an I/O stall.
- **Memory is fine:** `[mem 3] KiB free 54868 … pageouts 0` — 53 MiB free, no
  paging. The folder open is **not** memory pressure.
- So in QEMU the folder open is **CPU**: SpringBoard's animation + the CA
  render thread (texture uploads, command lists, the present). The `[smp]`
  sampler does not dump SpringBoard's threads (its task port is not
  reachable), so the per-frame cost is the `[perf]` histogram below.

## 3. The `[perf]` frame-time histogram (hardware side)

`ca_frame()` now measures the wall interval between CoreAnimation swaps on
the render thread (mach time) and, once per second while frames flow, prints
one histogram line. The interval is the real frame time: vsync pacing when
the compositor keeps up, longer when the CPU or the GPU (its own work or the
display mirror's) made it miss a refresh. It only prints during an animation
(a folder open/close, a scroll, a page load) and never on an idle screen.

```
[perf] CA: 42 frames in 1011 ms; frame ms min/avg/max 4/54/1277; <17:19 17-33:17 33-66:3 66-100:1 >=100:2
```

Buckets: `<17` ms = 60 fps, `17-33` = 30-60, `33-66` = 15-30, `66-100` and
`>=100` = dropped frames. A hardware log with this line shows, second by
second, exactly where a folder open or a Safari scroll drops frames. The
`[gpu] mirror: … fps` line (already in the log, every 30 s) gives the display
side of the same picture.

## 4. What was changed (this branch)

- **`[perf]` histogram** (§3, `tools/ios3ds-gl/ca.c`) — the hardware
  measurement for frame drops.
- **`emu/perf.py`** — the repeatable QEMU benchmark (folder open/close and a
  Safari cold launch; full page loads use `emu/safari_test.py`).
- **Cheaper CA render thread** (`tools/ios3ds-gl/pica_be.c`, `ca.c`):
  - the swizzle pass's command list and its six vertices are built **once per
    render target** and resubmitted each frame instead of being re-encoded on
    every present;
  - `ca_frame()` no longer calls `thread_info` (three Mach syscalls) on every
    swap — the render-thread CPU is sampled only on the reporting frames.

These are CPU-side trims; the dominant Old 3DS costs in §1 (the mirror's
~20 ms/frame and the GPU split with CoreAnimation) are GPU-side and need the
hardware `[perf]`/`[gpu]` lines to confirm, which is why §5 lists them.

Round 2 ([re-log.md](re-log.md) 113) attacks those GPU-side costs: the
mirror only runs after a present or a display-config change (damage tracking),
starts in the free slot right after a CoreAnimation present, and is told about
CPU-written surface frames through `PICA_OP_DIRTY`. The self-test and CA's
texture tiling were also made cheaper. QEMU's GPU model has no timing, so the
frame-rate gain has to be confirmed by the hardware histogram in §5.

## 5. What the next hardware run shows

| log line | checks |
|---|---|
| `[perf] CA: …` (new) | the real frame-time histogram while a folder opens/closes and while Safari scrolls: how many frames hit 60/30/15 fps and how many drop, on the Old 3DS |
| `[gpu] mirror: … fps` | the display copy rate next to it (the two should tell whether a drop is the compositor or the mirror) |
| `[gl] CA: … render thread CPU …` | the CA render thread cost on hardware (compare with re-log 78's 13.5-16 ms GPU-present / 31-37 ms read-back) |
| `[gl] pica: … reading surfaces back through the CPU` | whether any surface fell back to the 600 KiB/frame CPU read-back (re-log 78) |

With `diag=1` in `boot.cfg` the `[smp]`/`[mem]` lines are also present for
memory-pressure correlation.
