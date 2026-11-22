# Experimental stereoscopic 3D home screen

Status: **experimental, emulator only**. Off by default (`stereo3d=0`).
Enabled with `stereo3d=1` in `sd:/ios3ds/boot.cfg`.

## 1. Idea

The 3DS top LCD has a parallax barrier: with the barrier on it shows two
images, one per eye, and the viewer sees depth. iOS's home screen is drawn by
SpringBoard through CoreAnimation, which composites every layer with our GL
engine (`tools/ios3ds-gl`, docs/gpu.md). That engine already has the layer
geometry in hand, so it can composite the frame a second time for the other
eye with a small horizontal offset per layer depth, while the home screen is
visible:

- wallpaper furthest back,
- the icon grid at the convergence plane,
- dock and status bar in front of it,
- folders and alerts in front of those.

Offsets scale with the 3D slider (0 = flat 2D, barrier off) and are a few
pixels at most; the eyes shift in opposite directions. Everywhere except the
home screen (apps, the lock screen, alerts over apps) both eyes get the same
image or the barrier stays off. The bottom screen and the touch map do not
change.

## 2. Why the alpha channel

The composed frame is 32 bpp BGRA in window 1 (`/vram`, 0x18000000). Core
Animation's own colours, blending and coverage are left untouched in pass 1.
Pass 2 writes only the alpha channel of the same pixels with the layer's
depth class (a second, alpha-only pass over the same geometry, colour mask A
only, blending off, no texture). The ARM9 screen copy reads that byte back
and builds the two eye images by sampling the frame with a horizontal offset
that depends on the class.

The alpha values are chosen (0x00/0x33/0x66/0x99/0xCC) well away from the
premultiplied alphas a software compositor would leave behind (0x00
transparent, 0xFF opaque), so a frame without our stamps never looks like a
class map: `stereo_alphas_stereo()` rejects it.

## 3. Depth classes and offsets

Shared by the GL engine, the ARM9 copy, the kernel shim mirror and the host
tests in `boot/common/stereo.h`:

- `STC_FLAT` (0): both eyes identical,
- `STC_BG` (1): wallpaper, furthest back,
- `STC_ICONS` (2): icon grid, convergence plane,
- `STC_FRONT` (3): dock, status bar,
- `STC_ALERT` (4): folders, alerts, in front.

`stereo_eye_delta(cls, k, right)` gives the horizontal sampling offset in iOS
pixels for one eye: `out(x) = in(x - delta)`. Class 2 stays put; class 1
moves `-k` (left) / `+k` (right); class 4 moves `+2k` / `-2k`. `k` is
`stereo_shift_k(slider)`, 1..4 for slider 16..255 and 0 below that.

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

## 5. Cost

With the flag off the normal path is unchanged: `stereo_prepare()` returns -1
immediately and no stamp pass runs. With the flag on the back end logs frame
times in `ca_frame()` (`CA: ... frames; per frame ...`), including the extra
stamp passes; the wipe adds one full-screen quad to non-home frames. The
classifier and the texture opacity cache (`Tex.stereo_opq`, scanned once per
image) keep the overhead small.

## 6. QEMU

QEMU models the 3D slider as the MCU register 0x08 (`mcu-3d-slider` QOM
property). When the slider is up and the iOS framebuffer carries a class map,
the top screen is rendered as a red/cyan anaglyph built from the two eye
images, so the per-layer offsets are visible in a screenshot. With the slider
down or no class map (flag off, apps, lock screen) the normal top screen is
shown. The bottom screen and touch mapping are unchanged.
