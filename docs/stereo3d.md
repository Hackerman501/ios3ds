# Experimental stereoscopic 3D home screen

Status: **hardware path implemented, not yet tested on a console**. Off by
default (`stereo3d=0`). Enabled with `stereo3d=1` in `sd:/ios3ds/boot.cfg`;
the 3D slider must be above the bottom notch (MCU register 0x08 >= 16).
The bottom screen, the touch map and the GPU display mirror are unaffected
(the mirror is switched off by this flag: the ARM9 builds the eyes).

## 1. Idea

The 3DS top LCD has a parallax barrier: with the barrier on it shows two
images, one per eye, and the viewer sees depth. iOS's home screen is drawn by
SpringBoard through CoreAnimation, which composites every layer with our GL
engine (`tools/ios3ds-gl`, docs/gpu.md). That engine stamps the depth class
of the topmost layer into the frame's alpha byte while it composites:

- wallpaper furthest back,
- the icon grid at the convergence plane,
- dock and status bar in front of it,
- folders and alerts in front of those.

The ARM9 screen copy reads that class map and builds the left and right eye
images in the two LCD framebuffer sets, shifting each class by its eye offset.
The 3D slider scales the shift (0..4 pixels); slider down or a frame that is
not the home screen gives both eyes the same image and the barrier is off.
The alpha byte is masked away before the pixels reach the LCD: the screen
always gets opaque BGR8.

## 2. Why the alpha channel

The composed frame is 32 bpp BGRA in window 1 (`/vram`, 0x18000000). Core
Animation's own colours, blending and coverage are left untouched in pass 1.
Pass 2 writes only the alpha channel of the same pixels with the layer's
depth class (a second, alpha-only pass over the same geometry, colour mask A
only, blending off, no texture). The ARM9 screen copy reads that byte back.

The alpha values are chosen (0x00/0x33/0x66/0x99/0xCC) well away from the
premultiplied alphas a software compositor would leave behind (0x00
transparent, 0xFF opaque), so a frame without our stamps never looks like a
class map: `stereo_alphas_stereo()` rejects it. The ARM9 samples a 32x32 grid
of the frame's alpha bytes on every copy; one unstamped byte is enough to
fall back to the normal, flat copy.

## 3. Depth classes and offsets

Shared by the GL engine, the ARM9 copy, and the host tests in
`boot/common/stereo.h`:

- `STC_FLAT` (0): both eyes identical,
- `STC_BG` (1): wallpaper, furthest back,
- `STC_ICONS` (2): icon grid, convergence plane,
- `STC_FRONT` (3): dock, status bar,
- `STC_ALERT` (4): folders, alerts, in front.

`stereo_eye_delta(cls, k, right)` gives the horizontal sampling offset in iOS
pixels for one eye: `out(x) = in(x - delta)`. Class 2 stays put; class 1
moves `-k` (left) / `+k` (right); class 3 moves `+k` / `-k`; class 4 moves
`+2k` / `-2k`. `k = stereo_shift_k(slider)` is 1..4 for slider 16..255 and 0
below that. The frame is rotated on the LCD, so this horizontal iOS offset
becomes a vertical parallax shift on the 3DS panel.

## 4. GL engine stamping (`tools/ios3ds-gl/stereo.c`)

Only the CoreAnimation render server (`IOS3DS_CA=1`) ever stamps, and only
when the boot-arg `ios3ds_stereo=1` is present; otherwise the flag is ignored
and `gltest`/apps are unaffected. Each draw into the display target is
classified by `stereo_draw()`:

- the first full-screen opaque draw is the background (wallpaper, or an app
  window when CA culls what is behind it);
- a later full-screen opaque draw covers everything: the frame is flat (apps,
  the lock screen), nothing is stamped and the frame is wiped;
- a later full-screen translucent draw is a backdrop: it keeps the classes
  below it, and content drawn after it (a folder or alert) becomes `STC_ALERT`;
- a full-screen draw before any background is not stamped;
- everything else is classified by position: the status-bar band or the dock
  band => `STC_FRONT`, otherwise the icon grid => `STC_ICONS`.

A frame keeps its class map only if `stereo_frame_home()` holds: phase home or
alert, a background seen, and at least `STEREO_MIN_ICONS` (4) icon draws.
Frames that are not the home screen are wiped flat at the end of the frame
(one full-screen quad setting alpha to `SA_FLAT` everywhere), so the screen
copy sees a uniform frame and gives both eyes the same image.

Pass 1 is CoreAnimation's own draw, untouched. Pass 2 stamps alpha only.
`gl_be_triangles()` runs pass 1 then, when classified, pass 2.

## 5. Real hardware: the two-eye copy and the display registers

**ARM9 copy** (`boot/arm9/display.c`, `blit_top_stereo()`): one source row is
read per screen column as in the normal fast copy. For each output pixel the
alpha byte at that source position picks the depth class; the left eye
samples `x - delta(cls,k,left)`, the right eye `x + delta(cls,k,right)`,
clamped to the source width. Both samples are packed as BGR8 (low 24 bits
only) into `top_fb[0]` (left eye) and `top_fb[1]` (right eye). The bottom
copy and the touch map are the ordinary `blit_bottom()`, unchanged.

**PDC0 2D/3D switch** (`kernel/shim/pica_stereo.c`, ARM11 GPU part): the PDC
registers are ARM11-only, so the ARM9 publishes the requested mode in the
display mailbox (`gmbox_t.stereo_on/stereo_seq`, `boot/common/gpu_mirror.h`)
and the shim applies the preset on the next VBlank. Per 3dbrew
[GPU/External Registers](https://www.3dbrew.org/wiki/GPU/External_Registers):

- PDC0 (top screen) is at PA `0x10400400`;
- `+0x68/+0x6C` = framebuffer A (left eye) set 0/1, `+0x94/+0x98` =
  framebuffer B (right eye) set 0/1: both pairs are pointed at the two LCD
  framebuffer sets the ARM9 just wrote;
- `+0x70` bits 5-4 = 2 selects AB interlace (one scanline from A, the next
  from B) and bit 6 must be clear: this is the 3D mode GSP uses. The shim
  saves Luma's 2D preset on the first switch and restores it on the way back
  (bit 6 set, no interlace).

**Parallax barrier** (`boot/arm9/display.c`, ARM9 side). Per 3dbrew
[LCD Registers](https://www.3dbrew.org/wiki/LCD_Registers):

- parallax barrier enable at PA `0x10202000`: `0x00010001` on, `0` off;
- parallax barrier PWM at PA `0x10202004`: `0x0A390A39` before enabling
  (the value libn3ds' `LCD_setParallaxBarrier` writes).

The ARM9 enables the barrier only **after** the shim acknowledged the PDC
switch, and only when a stamped home frame was actually copied. Any later
flat frame (app, slider down, boot flag off) switches PDC0 back to 2D and
clears the barrier register again. No CONFIG11/SPI write is needed: the
barrier itself is the register above (3dbrew also notes that Horizon's GSP
turns it on when it programs AB interlace; we are baremetal, so we write it
directly).

**`stereo3d=0`** never calls any of this code: `display_blit()` executes the
same two copy calls as before, writing the same bytes to the same
framebuffers. `tools/ios3ds-pica/tests/test_stereo_copy.c` and
`test_mirror.c` check that byte-for-byte (including the flag-off case and
the slider-down fallback).

## 6. Cost

With the flag off the normal path is unchanged: `stereo_prepare()` returns -1
immediately and no stamp pass runs. With the flag on the CA render server
logs `CA stereo3D: N frames, N home (stamped), ...` and the usual
`CA: ... per frame ...` line in `sd:/ios3ds/log.txt` (enable `diag=1` for the
per-draw classifier log). The ARM9 logs each stereo transition and its
30-second `[monitor] CPU screen copy: ... ms each (ARM9)` average. The extra
cost is the alpha-only stamp pass on home frames, one wipe quad on other
frames, and writing both eye framebuffers instead of one; the GPU mirror is
off in this mode, so this replaces the GPU copy rather than adding to it.
Measured numbers from this branch are in docs/re-log.md, section 90.

## 7. QEMU

QEMU models the hardware registers, not the old shortcut: the anaglyph comes
from PDC0's framebuffer A (left) and B (right), the format bits at `+0x70`
and the barrier register at `0x10202000` (plus QOM properties `pdc-3d-mode`
and `lcd-3d-barrier`, readable with `qom-get /machine ...`). The arm9 gets an
alias of the LCD register page so its barrier writes reach the model. With
the barrier off or PDC0 in 2D mode the ordinary top-screen copy is shown.

The QEMU binary in `build/qemu` is shared between worktrees; this branch uses
its own private build (`QEMU_DIR=$PWD/build/qemu-hy4 emu/build-qemu.sh`, the
binary copied to `build/qemu-local/b/`), selected with
`IOS3DS_QEMU=$PWD/build/qemu-local/b/qemu-system-arm` and
`LD_LIBRARY_PATH=$PWD/build/qemu-local/b` (libslirp is bundled there)
(or `--qemu PATH` with either script; both pick up the bundled libslirp):

```
# flag on + slider up: anaglyph home screen; tap an app: no offset
LD_LIBRARY_PATH=$PWD/build/qemu-local/b \
IOS3DS_QEMU=$PWD/build/qemu-local/b/qemu-system-arm \
  python3 emu/run.py --time 90 --shots 70,80 --slider3d 255
# slider down: flat
... --hmp '5=qom-set /machine mcu-3d-slider 0'
# flag off: ordinary screen
```

`emu/soak.py --slider3d 255` sets the slider before boot for flag-on soaks.
The bottom screen and the touch mapping are unchanged.

## 8. What still needs a console

Everything above is implemented and exercised in QEMU, but **the 3D effect
has not been seen on a real 3DS**. To test:

1. Add `stereo3d=1` to `sd:/ios3ds/boot.cfg` (and keep `diag=1` if you want
   the per-frame classifier lines). Boot as usual.
2. On the home screen, push the 3D slider up and take a **photo of the top
   screen from straight in front**, with the slider at maximum and at the
   bottom notch (control photo). Compare: wallpaper/dock should separate in
   depth in the first, the second should look like 2D. Open an app with the
   slider still up and take a third photo: it must look flat.
3. Capture `sd:/ios3ds/log.txt` (and `log_prev.txt` if a previous run is
   relevant). The lines to look for are
   `[monitor] stereo3d=1: GPU display mirror off, ARM9 CPU copy builds the eyes`,
   `[gpu] PDC0 stereo preset applied: left fb ..., right fb ... (3D=1)`,
   `[monitor] stereo 3D: both eyes ... parallax barrier on (slider ..., shift ... px)`
   and, after leaving the home screen or sliding down,
   `[monitor] stereo 3D: top screen back to 2D, parallax barrier off`.
   Any `[monitor] stereo 3D: no shim ...` means the GPU part was not loaded
   (check `gpu=1`): the copy stays in 2D rather than showing a broken image.
4. Report the console model (Old/New, 2DS?) and whether the top LCD really
   shows depth. A 2DS has no barrier, so a flat picture there is expected.

Until that photo and log arrive, treat this as experimental; the README row
stays 🧪.

## Soak status at merge (2026-11-23)

- `stereo3d=0` (default): fresh-image 5-cycle `--icount 2` soak PASS (boot 85 s).
- `stereo3d=1`, slider 255, private QEMU build: boots (80 s), both eyes and the
  barrier come up (`[monitor] stereo 3D: both eyes ..., parallax barrier on`),
  no panic/stall/md0/crash, but the soak's timing checks FAIL: Home often takes
  effect after the 6 s check (recoveries "second Home", one "stuck"). In QEMU the
  ARM9's ~150 ms two-eye copy per frame shares the emulated time with iOS; on a
  console the ARM9 runs in parallel, so this is not a hardware prediction. Keep
  the flag experimental until a console test (section 8) says how it feels.
