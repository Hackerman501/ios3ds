# Camera: the 3DS cameras in iOS 4.2.1

Status: **on the first console run the sensors answer but no picture data
arrives (section 7); diagnostics and fallbacks are in for the next run.**
In QEMU Camera.app shows a live preview and takes photos through the whole
path (QEMU camera model → ARM9 sensor set-up → ARM11 FIFO drain → mediaserverd
plug-in → Celestial → Camera.app → Camera Roll). Video recording is not
offered (section 8).

```
 Camera.app / UIImagePickerController / AVFoundation (in the app)
      | FigRecorderRemote (Mach IPC "com.apple.coremedia.recorder")
 mediaserverd: Celestial FigRecorder -> FigLiveSource -> FigCaptureDevice/Stream
      |   figCreateVideoCaptureDevice loads /System/Library/MediaCapture/vcd.mediacapture
      |   (CFPreference "vcd" of com.apple.coremedia = true) before any Apple camera
 vcd.mediacapture (tools/ios3ds-camera/vcd, ours)          YUV422 -> BGRA, CMSampleBuffers
      | sysctl ios3ds_cam (CAM_OP_START / FRAME / STOP)       in CMBufferQueues
 kernel shim, GPU part (kernel/shim/cam.c)                   3 frame buffers, IOMallocContiguous
      | every 1 ms while a camera runs (audio service wake-ups): drain the CAM FIFO window
      | camera mailbox (shim + 0x7E00)
 ARM9 monitor (boot/arm9/camera.c): CFG11_CAMERA_CNT clock, Aptina MT9V113 set-up over I2C
 3DS: inner camera (I2C 0x10161000 dev 0x7A) and outer left camera (I2C 0x10144000 dev 0x78)
      -> CAM0 0x10120000 / CAM1 0x10121000, FIFO windows 0x10320000 / 0x10321000 (ARM11 only)
```

QEMU evidence (`--icount 2`, the camera model's pattern): live preview of the
outer camera, a photo, switch to the inner camera (two marker blocks), a photo,
Photos shows "Camera Roll (2)"; 144 frames per 10 s (14.4 fps), 0 FIFO
overruns, ARM9 sensor bring-up 5 ms per camera. Screenshots and the `[cam]`
log: `build/camera-evidence/` (not committed).

iOS sees two cameras: the 3DS **outer left** camera is the "back" camera
(`PortTypeBack`), the **inner** camera the "front" one (`PortTypeFront`, the
switch button in Camera.app). The outer right camera is kept in standby (it
shares CAM0's parallel bus with the inner one).

## 1. What iOS 4.2.1 needs (RE, re-log section 65)

| Fact | Where |
|---|---|
| Camera.app (`/Applications/Camera.app`, in the iPod2,1 rootfs) is tagged `SBAppTags = (still-camera)`; SpringBoard hides apps whose tags are not capabilities of the device | Camera.app Info.plist |
| The device capabilities come from `SpringBoard.app/N72AP.plist` `capabilities` (no camera keys on the iPod touch 2G); the iPod touch 4G's N81AP.plist has `still-camera`, `front-facing-camera`, `video-camera`, `hd-video-capture` | both rootfs images |
| Capture runs in **mediaserverd**: the app talks to `FigRecorderRemote`; the recorder, live source and capture devices are in Celestial inside mediaserverd (runs as `mobile`) | Celestial symbols, mediaserverd launchd plist |
| `figCreateVideoCaptureDevice` (Celestial 0x32e01520): first, if `CFPreferencesGetAppBooleanValue("vcd", "com.apple.coremedia")`, `dlopen("/System/Library/MediaCapture/vcd.mediacapture")` + `dlsym("FigVirtualCameraDeviceCreate")` (`FigLoadAspenCaptureDevice` 0x32e0133c); then IOKit `AppleH4CamIn`/`H3`/`H2`/`H1` → `H4ISP`...`H1ISP.mediacapture`. A loaded virtual device wins; create = `(allocator, NULL options, &device)` | disassembly |
| The capture device / stream interface is Apple's FigBaseObject: vtable `{0, base class, derived class}`, base class `{version 1, storage size, equal, invalidate, finalize, copyDebugDescription, copyProperty, setProperty}`, stream class `{1, start, stop}`; created with `FigDerivedObjectCreate(alloc, vtable, FigCapture{Device,Stream}GetClassID(), &obj)` | H3ISP.mediacapture (in the shared cache) 0x3e07e5b8.. |
| Device properties used: `StreamArray`, `Clock` (host clock), `SupportedPropertiesDictionary` | H3ISP device CopyProperty 0x3038c52c, Celestial |
| Stream properties used: `PortType` (`PortTypeBack`/`PortTypeFront`), `SupportedFormatsArray` (dicts of FormatDescription, VideoMaxFrameRate, VideoScaleFactor, VideoIsBinned), `FormatDescription`, `FormatIndex`, `Min/MaximumFrameRate`, `BufferQueue` and `StillImageBufferQueue` (CMBufferQueues **created by the plug-in**, Celestial installs triggers on them), `StillImageCaptureNow` (the next frame also goes to the still queue), `BufferAllocator`; notifications `StreamStarted`/`StreamStopped` on `FigCaptureDeviceGetNotificationCenter()` | H3ISP stream CopyProperty 0x30385a54, Celestial `livesource_updateSourceVideoQueues` 0x32e250e0 |
| Frames: `CMSampleBufferCreateForImageBuffer` of IOSurface-backed CVPixelBuffers (`kIOSurfaceIsGlobal`), PTS = host time in ns | H3ISP `MyH3ISPFrameReceivedProc` 0x3038afd0 |
| Four constants are private to Celestial (bound into Apple's plug-ins by the cache, not visible to `dlsym`): `PortType`, `PortTypeBack`, `PortTypeFront`, `FrameDroppedFromQueue`; they are compared with `CFEqual`, so equal CFStrings work | cache symbol tables |
| Modes and formats come from `Celestial.framework/<model>/AVCapture.plist`; N72's only has audio recording. Schema taken from the iPod touch 4G (N81) file: `AVCaptureMode_PhotoCapture`, `AVCaptureMode_FrontFacingPhotoCapture` with `LiveSourceOptions` `{VideoPort, Sensor, Capture, Preview (Width, Height, PixelFormatType), Min/MaxFrameRate, SensorOrientation, PreviewVideoMirror, FastSwitchCompanion}` | N81 rootfs (public firmware, looked at only) |
| CoreAnimation hands 4:2:0 IOSurfaces of a video layer to an AppleM2CLCD overlay window (`CADisplayIOMFB swap_queue`), which needs physically contiguous memory and is not shown by ios3ds: 420f previews were black (`AppleM2CLCD, swap_queue error: e00002c8`). BGRA previews are composited like any other layer | QEMU |
| Photos: `livesource_workerThread` → `FigAspenCreateJPEGFromSbuf` → no AppleJPEG service on N72 → `softwareJPEGEncode` (ImageIO `CGImageDestination`) | Celestial 0x32e25d40, 0x32e03b28 |

## 2. Choice of bridge

Options considered:

1. **A fake IOKit camera service** (`AppleH3CamIn` user client) under Apple's
   H3ISP.mediacapture: H3ISP drives an ISP with firmware upload, I2C, 3A and a
   frame receiver through dozens of user-client selectors. Emulating that in
   the shim is far more code than the camera itself, and none of it matches
   3DS hardware.
2. **Interposing CoreMedia/Celestial in mediaserverd** (like the Apple80211
   interposer): the capture device is created inside Celestial from a
   function table, not through calls an interposer could catch cleanly.
3. **Celestial's own virtual-camera slot** (`vcd.mediacapture`, chosen):
   Apple left a plug-in hook that is loaded before any hardware camera when one
   preference is set. The plug-in implements the same FigCaptureDevice
   interface as Apple's plug-ins, so everything above it (Celestial's live
   source, preview, still pipeline, JPEG, AVFoundation, UIImagePickerController,
   Camera.app) is Apple's unmodified code. No byte patch is needed anywhere.

Kernel/hardware side: the CAM data FIFO is ARM11-only, so the ARM11 must move
the data. The 3DS's own driver uses CDMA (peripheral 2/3); ios3ds drains the
FIFO with the CPU from the shim's interrupt path, every 1 ms while a camera
runs, the way the microphone FIFO is drained (docs/audio.md section 7). That
needs no DMA microcode and the QEMU model stays simple; the cost is bounded by
the frame size (section 6). The I2C buses and the camera clock are the ARM9's
(it already owns I2C for the MCU), so the sensor set-up is done there.

## 3. Pieces

| Piece | File |
|---|---|
| Plug-in `FigVirtualCameraDeviceCreate`: device with two streams, frame thread per running stream (sysctl → YUYV → BGRA IOSurface pool buffer → CMSampleBuffer → queues), stills, pattern mode (`ios3dsPattern` preference) | `tools/ios3ds-camera/vcd/vcd.c` → `/System/Library/MediaCapture/vcd.mediacapture` |
| Capabilities `still-camera`, `front-facing-camera` | `rootfs/recipe.json` `plist_merge` (new: nested-dictionary merge in `scripts/mkrootfs.py`) |
| Modes/formats: photo back + front (each the other's `FastSwitchCompanion`), 320x240 `BGRA`, 15 fps, `SensorOrientation 90`, no preview mirroring | `rootfs/files/camera/AVCapture.plist` → `Celestial.framework/N72/AVCapture.plist` (our own file; it replaces N72's audio-only one and keeps its audio mode) |
| `vcd = true` | `rootfs/files/camera/com.apple.coremedia.plist` → `/var/mobile/Library/Preferences/` |
| Shared contracts: sysctl requests, mailbox, register map | `boot/common/cam_proto.h` |
| Test pattern + YUV conversions (plug-in, QEMU model, host test) | `boot/common/cam_pattern.h` |
| Shim: sysctl `ios3ds_cam`, FIFO drain, overrun recovery | `kernel/shim/cam.c` (GPU part, `link.ld` `*cam*.o`), hooks in `gpu_glue.c`, `audio.c` |
| ARM9: camera clock, I2C (16-bit register index added to `i2c.c`), Aptina set-up, standby | `boot/arm9/camera.c` |
| boot.cfg `camera=1` (default) / `camera=0` | `scripts/mkboot.py`, `boot/arm9/main.c` |
| QEMU model | `emu/qemu/hw/arm/ctr_cam.c` |
| Host test | `tools/ios3ds-camera/tests/test_cam.c`, `tests/test_camera.py` |

### 3.1 Sensor set-up (ARM9)
GBATEK's DSi Aptina code list (reset, wakeup, PLL as on the DSi, YUV output,
sizes, AE window, sequencer refresh) with output size A = 320x240, then
"activate" (standby off, parallel output on). The read mode is GBATEK's
(0x24 for the 0x7A camera, 0x25 = x-flip for the others); which of them
gives a true or a mirrored picture is unverified (section 7). Each wait gives up after
100 ms; a camera that does not answer or does not come up reports
`CAM_ST_FAILED` and iOS shows a black preview. Switching cameras puts the
previous sensor into standby first.

### 3.2 Data (shim)
- START maps the CAM registers (0x10100000 section, shared with the audio
  code) and the FIFO windows (0x10300000 → VA 0xBFB00000), allocates three
  150 KiB frame buffers once, and posts the request to the ARM9.
- Once the ARM9 reports the sensor streaming, the interrupt path sets
  `CAM_CNT = 0x10 | 0x8000` (bit 4 must be set, bit 15 transfer; the transfer
  starts at a frame start) and from then on, every 1 ms, reads `CAM_STAT`'s
  fill count and copies that many words from the window into the frame being
  filled. Window reads always continue at the next address (a word is only
  consumed when the address changes).
- An overrun (stat bit 15) costs the frame: transfer off, overrun cleared,
  transfer on (next frame start), the partial frame dropped.
- FRAME copies the newest complete frame out (`copyout`), never one the
  interrupt is writing.

## 4. Preview, photos, orientation
- Preview: the plug-in delivers BGRA frames (YUYV converted in mediaserverd),
  which CoreAnimation composites like any layer. 4:2:0 frames go to a CLCD
  overlay window instead and stay black on ios3ds (section 1).
- Review image: Camera.app only saves a photo after Celestial has made its
  review image. Celestial uses the sample's `ScaledPreviewPixelBuffer`
  attachment as it is when there is one; otherwise it scales the frame to
  512x384 with VTPixelTransferSession, which on this device refuses to scale
  up (-12905) and the photo is silently dropped. The plug-in attaches each
  frame's own pixel buffer.
- Photos: on `StillImageCaptureNow` the next frame goes to the still queue at
  640x480, interpolated from the 320x240 frame (`cam_bgra_upscale2x`);
  Celestial JPEG-encodes it in software (no AppleJPEG service on N72) and
  Camera.app saves it into the Camera Roll (`/var/mobile/Media/DCIM`). The
  detail is that of the 320x240 sensor output.
- `SensorOrientation 90`: the 3DS sensor delivers landscape frames; iOS's
  portrait is the console turned 90°, like an iPhone's landscape sensor.
- Front/back switch: Celestial creates the device with the mode's stream and
  its `FastSwitchCompanion` stream and swaps them on the switch button
  (`livesource_swapVideoStreams`). The front preview is **not mirrored**:
  with `PreviewVideoMirror` Celestial needs a mirroring preview scaler
  (`livesource_setupVideoScalerForPreview`), which fails without the hardware
  scaler (-12914), and the switch ended in `PreviewFailed` (re-log 65).
  Without a companion Camera.app's switch button does nothing.

## 5. QEMU
`emu/qemu/hw/arm/ctr_cam.c`: three MT9V113 register models on I2C (answer only
while CFG11_CAMERA_CNT bit 0 is set), the two CAM blocks, their FIFO windows
(pop on address change, 0x500 units, overrun), and a moving test pattern at 15
fps on the virtual clock (`cam_pattern.h`: scrolling colour bars, a bouncing
square, one black block for the outer camera and two for the inner one).
`IOS3DS_CAM_LOG=1` logs register writes and transfers.

QEMU's default timing (virtual clock = wall clock while the guest runs about
3x slower than a 3DS) overruns the FIFO on every frame: use `--icount 2`
(real-hardware instruction timing), where the 1 ms drain keeps up with 0
overruns. The plug-in's pattern mode (`ios3dsPattern` = true in
com.apple.coremedia's preferences) bypasses the kernel path entirely.

## 6. Cost and limits
- 320x240 YUV422 is 150 KiB per frame: at 15 fps 288,000 FIFO words a second
  through the ARM11, a 150 KiB `copyout` and a YUYV → BGRA conversion per
  frame in mediaserverd.
- The FIFO holds about 4 ms of 320x240 video. While XNU masks interrupts longer
  than that (md0 transfers do, docs/architecture.md) frames are lost (counted
  as overruns, logged when the camera stops).
- Photos are 320x240. VGA would need 4x the FIFO bandwidth; with the CPU drain
  that is only reasonable at a few frames per second (or with CDMA).

## 7. Hardware: first run, diagnostics, what the log shows
**Run 23 (Old 3DS, re-log 67):** all three sensors answer (`chip id 2280`),
the outer left one comes up in ~135 ms, but `0 frames, 0 FIFO overruns`
every 10 s: no word ever reached the CAM1 FIFO. A third start timed out in
the sequencer refresh (`init failed (step 5)`). The shim's FIFO drain itself
is not in question yet: without overruns the FIFO never filled at all.

What the code does about it (all logged):
- **Both CAM blocks are watched** until data appears: if the left camera's
  data arrives on CAM0 instead of CAM1, the shim moves there
  (`CAM0`/`CAM1` in the stats line).
- **Set-up variants**, 2 s each, cycled until data appears
  (`kernel/shim/cam.c` `cam_var_*`): 0 = `CAM_CNT 0x8010`; 1 = plus `+6 =
  0x22` (GBATEK suspects +6 is the DSi's CAM_MCNT, whose "on" value is 0x22);
  2 = plus `CAM_CNT` bit 14 (data request) and bit 9 (IRQ).
- `CFG11_CAMERA_CNT` = 3 (bit 1 "reportedly turns the cameras on", GBATEK),
  read back and logged.
- Sensor waits 500 ms instead of 100 ms; the cameras are probed at boot (iOS
  saw "present 0x0" until the first start).

`[cam-diag]` lines in `sd:/ios3ds/log.txt`, in this order:
1. `[cam-diag] boot: CFG11_CAMERA_CNT .. CAM0 cnt .. stat .. +6 .. CAM1 ...`
   and the three `chip id` lines at boot.
2. `[cam-diag] CFG11_CAMERA_CNT xx -> yy`: whether bit 1 sticks.
3. After `streaming ... into CAM1`: the sensor registers (`STANDBY` bit 14 = in
   standby, should be 0; `RESET_MISC` bit 9 = parallel output, should be 1;
   `SEQ_STATE`: 3 = preview/streaming on the MT9V113 family; `OUT_W_A`/`OUT_H_A`
   = 0140/00f0).
4. At 0.2 s, 1 s, 5 s: both CAM blocks again (CAM_CNT as the shim set it), and
   per block over 50 ms: how often CAM_CNT bits 0/1 changed (GBATEK: they
   toggle during a transfer = the block sees sync from the sensor) and how
   often the FIFO fill changed.
5. Every 10 s: `shim: N services, M with data, stat ..., CAM_CNT ..., variant
   V, CAMp[, still probing: no data yet], W words`. `services` = 0 means the
   interrupt-time drain never ran for the camera (a shim problem, not the
   hardware's).

How to read it:
- Bits 0/1 toggle and the fill changes on one block → data arrives; the
  variant/port in the stats line is the working set-up (make it the default).
- No toggles on either block, sensor reports streaming (`STANDBY` 0, parallel
  bit set) → the CAM block is not clocked or not enabled: look at
  `CFG11_CAMERA_CNT` and the CAM_CNT readback (bits that did not stick).
- Sensor in standby / `SEQ_STATE` not 3 → the DSi code list does not bring the
  3DS sensor to streaming; the sensor side needs the official sequence.

Other unverified points: the colour byte order (GBATEK: Y0 U Y1 V), the
picture's orientation/mirroring (`SensorOrientation`, read mode 0x2717/0x272D
bit 0 = x-flip), and whether a 1 ms CPU drain keeps up on the console (many
overruns would mean CDMA is needed). `camera=0` in boot.cfg keeps the kernel
away from the cameras (the plug-in then reports no device).

## 8. Not done
- **Video recording**: Camera.app's video switch needs `video-camera` and an
  H.264 encoder; the iPod touch 2G has none (no `VideoEncoders` for H.264, no
  hardware), so the capability stays off.
- Focus, exposure, white balance and flash requests are accepted and ignored
  (the sensors run their own auto exposure/white balance).
- Mirrored front preview (section 4; the sensor's own x-flip could do it).
- The third (outer right) camera and 3D photos.
- VGA stills (section 6).
