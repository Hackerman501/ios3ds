# Input: 3DS touch panel and buttons into iOS 4.2.1

Status: **design + unwired code** (`tools/ios3ds-input/`). Nothing here is built
by the top-level Makefile, nothing has run on hardware or in QEMU yet. Every
binary-derived fact below was read out of the 8C148 dyld cache, SpringBoard
1288.40 or the kernelcache; the method is in the "RE log" section at the end.

## 1. Summary

```
 3DS hardware            ARM9 monitor                XNU shim               iOS userland
 ------------            ------------                --------               ------------
 HID_PAD 0x10146000 -+
                     +-> input_poll()  --inmbox_t--> sysctl               ios3ds-input daemon
 CODEC touch ADC    -+   (arm9/tsc.c)   @shim+0x6200  kern.ios3ds_input -> IOHIDEventSystemClient
 (NSPI bus 2, CS0)        change-only                 (shim/input.c)        DispatchEvent()
                          ring, 30 samples            read-only, 512 B          |
                                                                                v
                                                    SpringBoard's IOHIDEventSystem callback
                                                    (SBHIDInterface): hit-test, GSEvent hand
                                                    events to the right app, menu/lock/volume
```

| Layer | Choice | Why |
|---|---|---|
| Sampling | ARM9 monitor polls pad + CODEC over SPI | The ARM9 already polls `HID_PAD`, all needed registers are in the shared `0x101xxxxx` IO block, and no SPI traffic lands in an XNU interrupt handler |
| Transport | Ring in the shim page, exported as a read-only opaque sysctl registered by the shim | No new kernel patch (uses the `sysctl_register_oid` KPI); this kernel has no `/dev/kmem` |
| Injection | Daemon dispatches `IOHIDEvent`s to `com.apple.iohideventsystem` | Enters SpringBoard exactly where real hardware events do; no `GSEventRecord` building, no per-app port lookup, no kext |

## 2. 3DS side

### 2.1 Which CPU

Everything needed sits in the common IO block `0x10100000-0x101FFFFF`, which
both CPUs map (same block as `HID_PAD`, which `boot/arm9/main.c` already
reads; see `docs/research.md` section 1.1):

| Register | Address | Source |
|---|---|---|
| `HID_PAD` (active low, 12 buttons) | `0x10146000` | `boot/common/ios3ds.h`, libn3ds `source/arm11/drivers/hid.c` |
| `CFG11_SPI_CNT` (bit n: bus n+1 on the new NSPI interface) | `0x101401C0` | libn3ds `include/arm11/drivers/cfg11.h`, `spi.c` `NSPI_init` |
| NSPI bus 2 (`cnt,cs,blklen,fifo,fifo_stat,autopoll,int_mask,int_stat`) | `0x10142800` | libn3ds `include/arm11/drivers/spi.h` (`NSPI2_REGS_BASE`); linux-3ds `nintendo3ds.dtsi` `spi@10142800` |
| CTR CODEC ("TSC") = bus 2, chip select 0, 16 MHz | | libn3ds `spi.c` `g_spiDevTable[3]`; linux-3ds `tsc@0`, `spi-max-frequency = 16777216` |
| MCU (HOME / POWER / shell) = I2C bus 2, device `0x4A`, IRQ reg `0x10` | `0x10144000` | libn3ds `mcu.c`, `hid.c` `updateMcuHidState`; GodMode9 `arm9/source/system/i2c.c` |

So the ARM9 can do all of it, and should: the ARM11 runs XNU, where an SPI
transaction would have to live in the shim's VBlank IRQ path. The ARM9 loop
already spins at about 2 ms per iteration.

### 2.2 CODEC access and init

Framing (libn3ds `codec.c` `switchPage` / `readRegArray` / `writeReg`): select
a page by writing `{0x00, page}`; then byte 0 = `reg << 1 | read`, data follows
in the same chip-select window. NSPI transfer: `blklen = n`,
`cnt = 0x8000 | cs << 6 | clk (5 = 16 MHz) | 0x2000 if sending`, move 32-bit
words through `fifo`, wait for `cnt` bit 15 to clear, write `cs = 0`.

Is CODEC init needed? Yes, but only the touch part. After a plain FIRM boot
(our case: Luma chainloads us, Horizon's `codec` module never ran) the ADC is
not converting. GodMode9 (`arm11/source/hw/codec.c` `CODEC_Init`) and linux-3ds
(`drivers/platform/nintendo3ds/tsc/touch.c` `touch_initialize` +
`touch_enable`) both get a working panel from this alone, with no soft reset,
MCLK or sound setup:

```
page 0x67:  0x24=0x98  0x26=0x00  0x25=0x43  0x24=0x18  0x17=0x43  0x19=0x69
            0x1B=0x80  0x27=0x11  0x26=0xEC  0x24=0x18  0x25=0x53
enable:     0x26 |= 0x80;  0x24 &= ~0x80;  0x25 = (0x25 & ~0x3C) | 0x10
```

libn3ds `touchAndCirclePadInit` is the same sequence with the four constants
taken from HWCAL (its launch-day defaults give exactly `0x43 0x69 0x80 0x11`).
If a console needs more, the fallback is libn3ds' full `CODEC_init` prefix:
`PDN_I2S_CNT` (`0x10141220`) `|= 2` for MCLK, then soft reset (page 0x64 reg 1).

Reading (libn3ds `CODEC_getRawAdcData`, `CdcAdcData`): if page 0x67 reg 0x26
bit 1 is clear, read 0x34 bytes from page 0xFB reg 1: big-endian `u16
touchX[5], touchY[5], cpadY[8], cpadX[8]`. `touchX` bit 12 set means pen up;
the low 12 bits are the raw position (x: 0..4095 across the 320 px, y: 0..4095
down the 240 px; libn3ds and linux-3ds both scale linearly without
calibration). Circle pad: `(raw & 0xFFF) - 2048`, X inverted.

`tools/ios3ds-input/arm9/tsc.c` implements this with bounded busy-waits and a
read-back probe (`tsc_ok`), so a missing SPI model (our QEMU board: the block
is an `unimp` region that reads 0) disables touch instead of producing a
phantom pen-down at (0,0).

### 2.3 Buttons

`HID_PAD` gives A B SELECT START RIGHT LEFT UP DOWN R L X Y. HOME and POWER
only exist as MCU interrupt bits over I2C (reg `0x10`: bit 0 power pressed,
bit 2 HOME pressed, bit 3 HOME released); the monitor has no I2C driver yet,
so the first cut maps face buttons and reserves `INBTN_HOME` / `INBTN_POWER`:

| 3DS | iOS | HID usage (page 0x0C) |
|---|---|---|
| START (later also HOME) | Home / menu button | 0x40 |
| SELECT (later also POWER) | Lock / sleep button | 0x30 |
| R / L | Volume up / down | 0xE9 / 0xEA |
| SELECT+START | stays the monitor's "panic XNU" combo; not forwarded | |

### 2.4 Mapping the bottom panel onto the iOS screen

iOS is on the top screen as 360x240 with iOS y along the 3DS x axis and iOS x
along the 3DS y axis (`boot/arm9/display.c`: column = 20 + iy*3/4,
row-from-top = ix*3/4). The touch panel is the bottom screen (320x240), which
has the same axes in the book grip. Absolute, aspect-true map at 2/3 px per
iOS point:

```
ny = panel_x / 320                      (480 pt over the full 320 px)
nx = (panel_y - 13.33) / 213.33         (320 pt over 213 px, centred; margins clamp)
```

Recommended follow-up: let the ARM9 also blit iOS onto the bottom screen at
2/3 scale (213x320 inside 240x320) instead of the log. Then this same map is
**direct touch** (finger on the thing it hits) and the top screen is the large
view. Without that it works as an absolute tablet.

Note for whoever wires this: the current blit maps iOS (x, y) to 3DS
(row, column) = a transpose, which is a reflection rather than a rotation, so
the picture should look mirrored however the console is held. If that is
confirmed and fixed (`ix = rr*4/3`), start the daemon with `flipx`; the
touch map must mirror whatever the blit does.

### 2.5 Mailbox (`tools/ios3ds-input/input_proto.h`)

At `SHIM_PA + 0x6200` (free space between `blkmbox_t` at +0x6000 and the bounce
buffer at +0x8000), 512 bytes, written only by the ARM9:

```c
typedef struct {            /* 16 bytes */
    uint16_t buttons;       /* pressed = 1; bits 0-11 = HID_PAD bits, 12 HOME, 13 POWER */
    uint16_t flags;         /* bit0 pen down (tx/ty valid), bit1 circle pad valid */
    uint16_t tx, ty;        /* raw 12-bit ADC */
    int16_t  cx, cy;        /* circle pad, centred */
    uint32_t tick;          /* ARM9 poll counter */
} insample_t;

typedef struct {            /* 512 bytes */
    uint32_t magic;         /* 'INPT' 0x54504E49 */
    uint32_t version;       /* 1 */
    uint32_t head;          /* samples ever pushed, written last */
    uint32_t tsc_ok;        /* CODEC answered */
    uint32_t polls;         /* liveness */
    uint32_t _r[3];
    insample_t s[30];       /* ring; a sample is pushed only on a change */
} inmbox_t;
```

Change-only pushing (button edge, pen edge, move of 12 raw units or more)
means 30 slots cover any realistic gap between daemon polls.

## 3. Transport into userland

Options checked:

- **`/dev/kmem`**: not available. The kernelcache contains no `kmem`/`/dev/kmem`
  string at all (the `mm` device and its `kmem=1` boot-arg are compiled out),
  and the root image has an empty `/dev`.
- **`vm_read` on the kernel task** (`processor_set_tasks`, as `ios3ds-sample`
  does for user tasks): no patch needed, but whether the kernel map lets the
  shim's static physmap range be copied is unverified. Kept as a fallback.
- **Patched syscall slot**: needs the `sysent` address (not exported) and a
  new entry in `kernel/patches.json`.
- **Chosen: a sysctl registered by the shim.** `sysctl_register_oid` is an
  exported KPI, the OID struct lives in shim memory, and userland needs only
  `sysctl(2)`.

Kernel facts (8C148, xnu-1504.58.28):

| Symbol | Address | Notes |
|---|---|---|
| `sysctl_register_oid` | `0x80159c54` (Thumb) | takes the kernel funnel and the sysctl geometry rw-lock (allocates it if NULL); `oid_number == -1` becomes max(100, existing)+1. Thread context only |
| `sysctl_handle_opaque` | `0x80159242` (Thumb) | `(oidp, arg1, arg2, req)` -> `sysctl_io_opaque(req, arg1, arg2, NULL)` |
| `sysctl__kern_children` | `0x8027f7b8` | parent list for `kern.*` |

`struct sysctl_oid` in this kernel is the 36-byte layout (no descr/version):
`{parent, link, number, kind, arg1, arg2, name, handler, fmt}`; e.g. `kern.nbuf`
at `0x80257380` = `{0x8027f7b8, 0, -1, 0x80000002, &nbuf, 0, "nbuf",
sysctl_handle_int|1, "I"}`.

`tools/ios3ds-input/shim/input.c` registers `kern.ios3ds_input`
(`CTLFLAG_RD | CTLTYPE_OPAQUE` = `0x80000005`, arg1 = `0x80846200`, arg2 = 512).
Its handler invalidates the 16 D-cache lines of the mailbox (the ARM9 writes
behind the ARM11's cache, as with `blkmbox_t`) and calls
`sysctl_handle_opaque`. Registration is one call from thread context; the first
`shim_mdevstrategy()` (root mount) is the suggested place.

The daemon polls it every 16 ms (one 512-byte copyout). If that proves too
chatty, the next step is a blocking read (handler sleeps until `head` moves,
woken from the shim's VBlank IRQ).

## 4. iOS side

### 4.1 Chosen: IOHIDEventSystem client dispatch

SpringBoard's launchd job declares the Mach services `PurpleSystemEventPort`
and `com.apple.iohideventsystem`. IOKit's client API reaches the latter with
no entitlement check:

- `IOHIDEventSystemClientCreate` (`0x337fd5fc`) -> `bootstrap_look_up`, then
  `io_hideventsystem_open` (MIG id 70000, blocks until SpringBoard answers);
  the returned per-client port is stored at `client + 8`.
- `IOHIDEventSystemClientDispatchEvent(client, event)` (`0x337fcad8`)
  serialises the event (`IOHIDEventCreateData`) and sends
  `io_hideventsystem_dispatch_event`.
- Server side `__io_hideventsystem_dispatch_event` (`0x33808fc4`) rebuilds the
  event and calls `_IOHIDEventSystemDispatchEvent` (`0x337fb2e8`) ->
  `__IOHIDEventSystemEventCallback(system, NULL, NULL, event)`, which invokes
  the callback given to `IOHIDEventSystemOpen` (rate limiting is skipped when
  the service is NULL).

SpringBoard's callback (`0x51978`, SBHIDInterface.m) switches on the event type:
3 keyboard -> `0x5028c`, 11 digitizer -> `0x50e14`, 13 accelerometer, 14
proximity. A NULL service is fine for all of them.

**Digitizer handler (`0x50e14`)** expects a parent with children:

| Field | Parent | Each child (type 11) |
|---|---|---|
| `0xB0007` event mask | bit 0 range, bit 1 touch, bit 2 position, bit 7 cancel | bit 7 = cancelled path |
| `0xB0009` touch / `0xB0008` range | any finger down | this finger (counted) |
| `0xB0000` / `0xB0001` x, y | normalised 0..1 | normalised 0..1 |
| `0xB0005` index, `0xB0006` identity | | identity must be <= 5 |
| `0xB000A` pressure, `0xB0014` major radius | | copied into the path |

Position = `min(round(n * pixels), pixels - 1) / scale`, with pixels and scale
from `[UIScreen mainScreen]`, so no multitouch hardware properties are needed.
State machine: not tracking + touch + mask&2 -> hand type 1 (down); tracking +
touch + mask == 4 -> 2 (drag); tracking + no touch + mask&2 -> 6 (up); a change
in the number or identity of touching paths -> 5. The up event must still carry
its child. On down it asks the CAWindowServer display for
`clientPortAtPosition:` / `contextIdAtPosition:`, falls back to
`GSGetPurpleSystemEventPort()` for SpringBoard's own windows, and sends the
hand event with `GSSendEvent`. All of it is skipped while SpringBoard's "touch
disabled" flag (`0x18d3fd`, e.g. display asleep) is set; a button event wakes it.

**Keyboard handler (`0x5028c`)**, usage page `0x0C`: `0x40` menu (GSEvent
1000/1001), `0x30` lock (1010/1011), `0xE9` / `0xEA` volume up / down
(1006/1007, 1008/1009). Telephony-page usages (headset button, ringer switch)
are handled there too but were not mapped out.

Event constructors (armv6 APCS: the 64-bit timestamp is in r1:r2 with no pair
alignment; `IOHIDFloat` is a 32-bit float in core registers, stored 16.16;
Booleans are read as whole words):

```c
IOHIDEventCreateDigitizerEvent(alloc, u64 ts, u32 transducer, u32 index, u32 identity,
    u32 eventMask, u32 buttonMask, float x, float y, float z, float tipPressure,
    float barrelPressure, Boolean range, Boolean touch, u32 options);      /* 0x337fa3a8 */
IOHIDEventCreateDigitizerFingerEvent(alloc, u64 ts, u32 index, u32 identity,
    u32 eventMask, float x, float y, float z, float tipPressure, float twist,
    Boolean range, Boolean touch, u32 options);   /* 0x337fa568; radii default to 5.0 */
IOHIDEventCreateKeyboardEvent(alloc, u64 ts, u16 usagePage, u16 usage,
    Boolean down, u32 options);                                            /* 0x337fa9dc */
IOHIDEventAppendEvent(parent, child);                                      /* 0x337fa114 */
```

`tools/ios3ds-input/daemon/input.c` does exactly this: down = mask 3 + touch,
move = mask 4, up = mask 3 without touch, one finger child (index 1,
identity 2). It exits when the port at `client + 8` becomes a dead name
(SpringBoard restarted) and lets launchd's KeepAlive reconnect it.

### 4.2 Fallback: GSEvents (option a)

Not needed, but the layout fell out of the same disassembly (SpringBoard builds
it on the stack in `0x50e14`; `GSEventGetHandInfo` `0x35260af0` and
`GSEventGetPathInfoAtIndex` `0x35260b10` confirm it). A `GSEventRef` is an
8-byte CFRuntimeBase followed by the record; `GSSendEvent(const GSEventRecord
*, mach_port_t)` (`0x352633f4`) and `GSSendSystemEvent(record)` (`0x352635a4`,
sends to `GSGetPurpleSystemEventPort()`) take the record **without** the CF
header. Offsets below are record-relative (add 8 for a `GSEventRef`):

```c
struct GSEventRecord {          /* 0x34 bytes */
    uint32_t type;              /* 0x00  3001 (0xBB9) = hand; 1000/1001 menu down/up;
                                         1006-1009 volume; 1010/1011 lock down/up */
    uint32_t subtype;           /* 0x04 */
    float    location[2];       /* 0x08  CGPoint */
    float    windowLocation[2]; /* 0x10  CGPoint */
    uint32_t windowContextId;   /* 0x18 */
    uint64_t timestamp;         /* 0x1C  mach_absolute_time (unaligned) */
    void    *window;            /* 0x24 */
    uint32_t flags;             /* 0x28 */
    uint32_t senderPID;         /* 0x2C */
    uint32_t infoSize;          /* 0x30  0x24 + 0x18 * pathCount for hand events */
};
struct GSHandInfo {             /* 0x24 bytes, at record + 0x34 */
    uint32_t type;              /* 0x00  1 down, 2 dragged, 5 path set changed, 6 up */
    uint16_t deltaX;            /* 0x04  SpringBoard: touch count at down */
    uint16_t deltaY;            /* 0x06  SpringBoard: current touch count */
    float    _unk[6];           /* 0x08 */
    uint8_t  eventMaskHi;       /* 0x20  HID event mask >> 24 */
    uint8_t  pathInfosCount;    /* 0x21 */
    uint8_t  _pad[2];
};
struct GSPathInfo {             /* 0x18 bytes each, at record + 0x58 */
    uint8_t  pathIndex;         /* 0x00 */
    uint8_t  pathIdentity;      /* 0x01 */
    uint8_t  pathProximity;     /* 0x02  bit0 in range, bit1 touching */
    uint8_t  _pad;
    float    pathPressure;      /* 0x04 */
    float    pathMajorRadius;   /* 0x08 */
    float    pathLocation[2];   /* 0x0C  CGPoint, points */
    void    *pathWindow;        /* 0x14 */
};
```

`GSSendSimpleEvent(type, port)` (`0x35263510`) sends a zeroed 0x34-byte record
with just the type (`GSEventLockDevice` = type 1014 to the system port). Why this is second choice: a userland sender must find the
frontmost app's event port itself for touches outside SpringBoard, and it
bypasses SpringBoard's tracking state.

### 4.3 Rejected: fake kernel HID service (option b)

Needs a kext (IOHIDEventService subclass, C++ runtime, prelink or kext loading)
and we have no kext toolchain. It buys nothing over 4.1, which enters the same
callback.

## 5. Files

| File | What |
|---|---|
| `tools/ios3ds-input/input_proto.h` | `inmbox_t` / `insample_t`, offset `0x6200`, sysctl name |
| `tools/ios3ds-input/arm9/tsc.c`, `tsc.h` | NSPI + CODEC touch init/read, `input_poll(shim_pa)` producer |
| `tools/ios3ds-input/shim/input.c` | `shim_input_register()`, sysctl handler |
| `tools/ios3ds-input/daemon/input.c` | the iOS daemon |
| `tools/ios3ds-input/daemon/Makefile` | standalone build via `scripts/ios-cc` |
| `tools/ios3ds-input/daemon/com.ios3ds.input.plist` | launchd job (KeepAlive, root) |

Compile checks done (objects/binary written outside the repo): `arm9/tsc.c`
with the boot ARM9 flags, `shim/input.c` with `-mcpu=mpcore -marm`, the daemon
with `scripts/ios-cc` (links IOKit + CoreFoundation stubs; the call sites were
disassembled and match the callee stack layouts above).

## 6. Wiring checklist (not done)

1. `boot/common/ios3ds.h`: move the contents of `input_proto.h` in.
2. `boot/Makefile`: add `tsc.c` to the ARM9 objects; `boot/arm9/main.c` monitor
   loop: `if ((tick & 7) == 0) input_poll(cfg.shim_pa);`.
3. `scripts/mkboot.py` `build_shim()`: add `input.c` to the gcc command;
   `kernel/shim/blk.c`: call `shim_input_register()` at the top of
   `shim_mdevstrategy()`. Check `__shim_end` stays below `0x80846000`.
4. `rootfs/recipe.json`: add the binary and the plist the way
   `ios3ds-sample` is added.
5. QEMU board (`emu/qemu/hw/arm/nintendo3ds.c`): model NSPI bus 2 + the CODEC
   pages 0x67/0xFB and feed it from the mouse, otherwise only buttons can be
   tested in emulation (the probe correctly reports `tsc_ok = 0` today).
6. Later: MCU I2C on the ARM9 for HOME/POWER; bottom-screen mirror for direct
   touch; a timer-based 60 Hz poll cadence instead of loop ticks.

## 7. Open risks

- Untested end to end. The first things to check on a run: the daemon's
  "connected" line, then SpringBoard's own logging (`SBHIDInterface` prints
  "Menu Button Pressed" etc. when its debug level is on).
- `sysctl_register_oid` from the first `mdevstrategy` call assumes that call is
  in a blockable thread context (it is the root mount in `bsd_init`).
- SpringBoard's "touch disabled" flag: if the display is considered asleep,
  touches are dropped until a menu/lock key event wakes it.
- The CODEC init is the GodMode9/linux-3ds minimal sequence; it has not been
  tried from the ARM9 on this boot path.
- Display orientation (section 2.4) decides `flipx`.

## 8. RE log

Tools: `dc.py` (dyld cache symbol lookup + disassembly), a SpringBoard
disassembly listing, capstone over `kernel.macho` with the kernelcache symbol
table, `hfsplus` to pull plists out of `rootfs.hfs`.

1. Listed IOKit HID symbols in the cache: `IOHIDEventSystemClientDispatchEvent`
   exists (`0x337fcad8`). Disassembled it, `io_hideventsystem_dispatch_event`,
   the server routine `__io_hideventsystem_dispatch_event`,
   `_IOHIDEventSystemDispatchEvent`, `__IOHIDEventSystemEventCallback` and
   `__IOHIDEventSystemIsTimeToDispatchEvent`: a client-dispatched event reaches
   the host's callback with service = NULL and is never rate-limited.
   `__io_hideventsystem_open` has no audit-token or entitlement check.
2. `com.apple.SpringBoard.plist` MachServices: `PurpleSystemEventPort`,
   `com.apple.iohideventsystem`.
3. SpringBoard: found `IOHIDEventSystemCreate` / `IOHIDEventSystemOpen` at
   `0x50c48` / `0x50c80`, callback `0x51978`; read the type switch and the
   digitizer (`0x50e14`), keyboard (`0x5028c`) and coordinate (`0x4f818`)
   routines; screen size/scale globals `0x18d438` / `0x18c5d4` are set from
   `UIScreen` at `0x50b04`.
4. Stack frame of `0x50e14` gave the GSEvent hand layout (record at sp+0x10,
   hand info at +0x34, 24-byte paths from +0x58, `infoSize = 24n + 0x24`,
   type `0xBB9`); cross-checked with `GSEventGetHandInfo` (copies 0x24 bytes
   from object+0x3C), `GSEventGetPathInfoAtIndex` (object+0x60 + 24i),
   `_GSEventGetGSEventRecord` (object+8), `GSSendSimpleEvent` (0x34 zero bytes).
5. Disassembled the three `IOHIDEventCreate*` constructors for their argument
   layout; compiled the daemon and compared its call sites.
6. Kernel: no `kmem` string anywhere in the kernelcache; `sysctl_register_oid`
   disassembled (funnel, lock, OID_AUTO numbering, no version check); scanned
   `__DATA` for OIDs whose parent is `sysctl__kern_children` to get the 36-byte
   struct layout and flag values.
7. 3DS: read libn3ds `codec.c`, `spi.c`, `hid.c`, `hw_cal.c`; linux-3ds
   `ctr_tsc.c`, `tsc/touch.c`, `nintendo3ds.dtsi`; GodMode9 `arm11/source/hw/codec.c`
   (minimal init that works after a FIRM boot) and `arm9/source/system/i2c.c`
   (the ARM9 can reach the MCU).
