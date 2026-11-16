# PICA200: GPU-accelerated OpenGL ES 1.1

`tools/ios3ds-gl` (see [gpu.md](gpu.md)) draws through a `struct Backend`.
This document describes the second back end, which renders on the 3DS's
PICA200 instead of the CPU, and everything underneath it.

**Status.** The whole path runs and is pixel-compared against the software
rasterizer, but only against a *software model* of the GPU (on the build host
and inside our QEMU machine). It has not run on a real 3DS yet. For that
reason the software rasterizer is still the default; the GPU back end is
selected per GL context with `IOS3DS_GL=pica` (§6). §8 is the checklist for
the first run on hardware.

## 1. Layers

```
app / QuartzCore ── gl* ──> OpenGLES.framework ──> MBXGLEngine (tools/ios3ds-gl)
  api.c, draw.c     GL state, transform + lighting on the CPU, clip-space triangles
  pica_be.c         Backend "ios3ds PICA200": batches draws into command lists
  tools/ios3ds-pica/src
    pica_cmd.c        command-list encoder
    pica_state.c      register encoders: target, viewport, depth/blend/alpha, textures, TEV, vertex layout
    pica_ff.c         fixed function: vertex shader + uniforms, glTexEnv -> TEV, GL enum mapping
    pica_tex.c        texel conversion and 8x8 Morton tiling
    pica_dev_ios.c    transport: sysctl ios3ds_gpu
──────────────────────────────── kernel ────────────────────────────────
  kernel/shim/pica.c  the sysctl node; GPU bring-up; arena; copyin/copyout; cache maintenance
  pica_drv.c          driver core: GX registers, allocator, PSC fill, PPF transfer, P3D submit
──────────────────────────────── hardware ──────────────────────────────
  PICA200 GX block at PA 0x10400000        (QEMU and host tests: tools/ios3ds-pica/sim/picasim.c)
```

`pica_drv.c` is freestanding C behind a small platform table. The shim links
it against real registers; the host tests link the very same file against
`picasim` (`pica_dev_host.c`). `pica_be.c` is identical in both builds too.

## 2. How userland reaches the GPU

The PICA200 only understands physical addresses, and iOS userland has no way
to get physically contiguous memory or map device registers. Two designs were
considered:

1. **Map** the GX registers and an arena into every GL process and let the
   engine drive the GPU itself (what the earlier draft of `tools/ios3ds-pica`
   assumed). Fast, but it needs `IOMemoryDescriptor` C++ calls from the shim, a
   cross-process lock and allocator in shared memory, and any GL process could
   make the GPU write anywhere in RAM.
2. **A service in the shim** (chosen): userland sends requests, the shim owns
   the registers and the arena and copies data in and out.

The service is the top-level sysctl node `ios3ds_gpu`, registered by the shim
like `kern.ios3ds_input`. A request is *one extra MIB component*: the address
of a `pica_req_t` in the caller (`tools/ios3ds-pica/include/pica_proto.h`).
`sysctl_root` hands a `CTLTYPE_NODE` handler the unconsumed part of the name,
so the handler gets that address, `copyin`s the request, runs it and
`copyout`s the result. Nothing is passed as a sysctl "new value", so only
sysctl *read* permission is needed. Reading the node plainly
(`sysctl ios3ds_gpu`) returns a `pica_info_t` without side effects.

The node is not under `kern.`: with `kern.ios3ds_gpu` the first QEMU run
could not reach the service (the engine logged "no service" and fell back to
the software rasterizer, as designed). The errno was not captured in that run.
The likely cause, from the xnu-1504 source: `__sysctl` sends every `CTL_KERN`
name through the old `kern_sysctl()` first, which returns `ENOTDIR` for a name
with more than one component after `kern` unless that component is on a fixed
list. A top-level node goes straight to the OID tree, and works.

| op | effect |
|---|---|
| `INFO` | bring-up on first use; version, state, `PDN_GPU_CNT`, arena address/size/free, counters |
| `ALLOC` / `FREE` / `FREE_ALL` | arena blocks (128-byte granules), owned by the calling pid |
| `WRITE` / `READ` | copy between a user buffer and arena memory (+ cache clean / invalidate) |
| `SUBMIT` | copy a command list into the driver's staging buffer (optionally vertex data first), run it, wait for the end marker |
| `FILL` | PSC memory fill, two slots |
| `TRANSFER` | PPF display transfer |

One request runs at a time (a flag taken with interrupts masked; waiters
`IOSleep`). Blocks of processes that died are reclaimed when the arena runs
full (`proc_find`). Completion is **polled**: `PSC_FILLn_CNT` bit 1, `PPF_CNT`
bit 8, and bit 31 (P3D) of the GX interrupt status register at +0x34, every
20 µs, for at most 2 s. A timeout is logged with the registers involved and
returned as `PICA_ERR_TIMEOUT`; the engine then turns the GPU path off for the
process and continues on the software rasterizer.

The price of design 2 is one copy per texture upload, per vertex batch and
per frame read-back. Interrupt-driven waits (P3D is GIC 45; `shim_irq` in
`entry.S` already has the trampoline) and mapping the arena into clients are
the obvious next optimisations; neither changes the protocol's shape.

Not done: validating the *contents* of command lists. A list can point the
GPU's render target at any physical address. ios3ds already runs with code
signing off, so this is not a new hole, but it should be closed (scan for the
address registers) before anything untrusted may use GL.

### GPU memory: the arena

- VRAM (0x18000000, 6 MiB) is taken: `/vram` gives iOS the first 3 MiB for the
  CLCD framebuffer, Luma's LCD framebuffers (which the ARM9 draws) start at
  0x18300000.
- So the arena is FCRAM: `IOMallocContiguous(8 MiB, page aligned, &pa)` at the
  first request (falling back to 4 and 2 MiB). It is ordinary wired, cached
  kernel memory; the GPU sees it by physical address. `CFG11_GPUPROT`
  (0x10140140) is written 0 so GPU DMA may reach all of FCRAM.
- Cache: after the CPU writes arena memory the shim cleans those D-cache
  lines (`c7,c10,1`), and before it reads memory the GPU wrote it invalidates
  them (`c7,c6,1`). Blocks are multiples of 128 bytes, so a line never holds
  two owners' data.
- The first 128 KiB are the driver's command-list staging buffer.

### Power and clock

`PDN_GPU_CNT` (0x10141200) must read 0x1007F: bit 16 clock enable, bits 0-6
"not in reset" for the register block, PSC, geometry, rasterizer, PPF and the
two PDC blocks (libn3ds `pdn.h`). Luma leaves it like that whenever it
initialised the screens, which ios3ds requires anyway. The shim checks it at
bring-up and logs the value; if it differs it enables the clock and releases
the resets (as libn3ds `PDN_controlGpu` does) and says so. It never *resets*
the block: the display controllers in it are live.

`pica_drv_init` then does the part of libn3ds `hardwareReset()` that concerns
us: PSC and PPF control registers to 0, `IRQ_ACK`, `IRQ_CMP = 0x12345678`,
`IRQ_MASK`, `IRQ_AUTOSTOP = 1`, `START_DRAW_FUNC0 = 1` (configuration mode;
"if not set the first command list will hang the GPU"), and one throw-away
16-byte texture copy (libn3ds: PPF glitches if the first operation after a
reset is a real one).

## 3. What the back end does (`tools/ios3ds-gl/pica_be.c`)

**Vertices.** The front end still transforms and lights on the CPU, so the
GPU gets clip-space `Vtx` records. `struct Vtx` *is* the vertex buffer format:
four float attributes (position 4, colour 4, texcoord0 2, texcoord1 2) and 4
bytes of padding, stride 52. The vertex shader (`shader/ff.vsh`, assembled by
`picasm.py`) multiplies the position by a "clip matrix" and the texture
coordinates by two texture matrices. Here the clip matrix only
- maps GL's clip z (−w…w) to the GPU's (−w…0), and
- applies a viewport correction when the GL viewport does not fit the GPU's
  register (negative origin, larger than the target): the GPU viewport then
  becomes the whole target and the scissor box does the clipping.

The texture matrices scale [0,1] to the used corner of a power-of-two
texture. Only non-indexed triangle lists are drawn (`DRAWARRAYS`); citro3d's
register sequence is used.

**Render targets.** The GPU renders into 8×8-tiled buffers; iOS surfaces are
linear BGRA. Per render target (keyed by `rt->color`) the back end keeps a
tiled RGBA8 colour buffer and a linear staging buffer in the arena, padded to
multiples of 16 and at least 64×64 (smaller display transfers are unreliable
on hardware, libn3ds). Depth is D24S8, GPU-only, keyed by `rt->depth`.
- *CPU → GPU* (first draw after the CPU copy changed, unless the draw is a
  full clear): rows are written to the staging buffer and a display transfer
  with "output tiled" converts them.
- *GPU → CPU* (`finish`: `glFinish`/`glFlush`, `glReadPixels`, present, or
  switching to another target): display transfer tiled → linear, `READ`,
  then one CPU pass that rotates each word (GPU RGBA8 word is
  `R<<24|G<<16|B<<8|A`, the engine's is `A<<24|R<<16|G<<8|B`) and maps rows.
  The GPU stores GL row 0 last, which is also how `flip` targets (IOSurfaces)
  are laid out.
- Only one target is ever ahead of its CPU copy. `gl_be_forget()` (called
  where the engine frees renderbuffer or texture storage) drops GPU copies of
  memory that is going away.

**Clears.** A full clear is a PSC memory fill of the colour and/or depth
buffer. A scissored or channel-masked clear is a quad at the clear depth with
depth function ALWAYS.

**Textures.** Every texture is uploaded as tiled RGBA8, padded to a power of
two (edge texels replicated), when its `be_seq` changes.

**State.** All register state of a draw is collected in one struct and sent
when it differs from the last one in the list: viewport, scissor, cull mode,
depth test/func/mask/range, polygon offset (constant term only), alpha test,
blend equation and factors, colour mask, two texture units (filter, wrap),
and the TEV stages. `glTexEnv` maps to one TEV stage per unit: `MODULATE`,
`REPLACE`, `DECAL`, `BLEND`, `ADD` depending on the texture's base format,
and all of `GL_COMBINE` (sources, operands, functions, scale) directly.

**Batching.** Draws accumulate in one command list (≤ 6144 vertices, 96 KiB)
until a flush: `finish`, target change, texture upload, full clear, or a full
buffer. Each list is self-contained (shader, base state, target), so several
GL processes can share the GPU without tracking each other.

**Fallback.** If the GPU is unavailable, a target is larger than 1024, the
arena is full or a GPU operation fails, the same draw goes to the software
rasterizer, and one `[gl] pica: …` line says why.

Not done yet: fog (drawn without, logged once), stencil, mipmaps, the slope
term of polygon offset, NPOT textures with REPEAT wrap, flat shading,
textures or targets above 1024. T&L stays on the CPU.

## 4. Register facts and where they come from

Nothing here was measured on hardware yet. Sources: libn3ds
(`include/arm11/drivers/gx.h`, `gpu_regs.h`, `pdn.h`, `source/arm11/drivers/gfx.c`),
libctru/citro3d register names and write sequences, and the Citra/Azahar
software renderer (`src/video_core/renderer_software/sw_rasterizer.cpp`,
`sw_clipper.cpp`, `sw_framebuffer.cpp`, `sw_blitter.cpp`, `pica/pica_core.cpp`)
for semantics. No code was copied from any of them; picaGL was not used.

| fact | source |
|---|---|
| command word pair: value, header (reg 0-15, byte mask 16-19, extra count 20-27, bit 31 consecutive) | Citra `ProcessCmdList` |
| list ends at a `FINALIZE` (0x010) write with a byte equal to `IRQ_CMP`; sets GX stat bit 31 | Citra `HandleSpecialReg`, libn3ds `GX_processCommandList` |
| submit: `CMDBUF_SIZE0 = bytes>>3`, `CMDBUF_ADDR0 = pa>>3`, `CMDBUF_JUMP0 = 1` | libn3ds |
| clip volume −w ≤ x,y ≤ w, −w ≤ z ≤ 0; depth = z/w · scale + offset | Citra clipper / rasterizer |
| cull mode 2 keeps counter-clockwise (positive area, y up), 1 keeps clockwise | Citra `ProcessTriangle` |
| colour/depth buffers and textures are 8×8 Morton tiles; the first stored row is the top (GL) row | Citra `DrawPixel`, `LookupTexture` |
| RGBA8 pixel bytes are A,B,G,R | Citra `EncodeRGBA8` |
| colour buffer formats number RGB5A1 = 2, RGB565 = 3; PPF formats RGB565 = 2, RGB5A1 = 3 | Citra `regs_framebuffer.h` vs `PixelFormat` |
| display transfer flags: bit 0 flip, bit 1 linear → tiled, bit 5 raw, bits 8-10 / 12-14 formats | libn3ds `gx.h`, Citra `sw_blitter.cpp` |
| PSC fill: start>>3, end>>3, value, control (bit 0 start, bit 1 done, bits 8-9 width) | libn3ds, 3dbrew |
| TEV stage 0 has no "previous" | Citra `WriteTevConfig`; we always name the vertex colour explicitly |
| vertex colours are \|c\| saturated before interpolation | Citra `output_vertex.cpp` |

## 5. The software GPU model (`tools/ios3ds-pica/sim/picasim.c`)

QEMU has no PICA200, and porting Citra's C++ core into a C board file was more
work than writing the subset we emit. `picasim` (about 900 lines of C,
GPL-2.0-or-later so it can be linked into QEMU) models the GX block at
register level: PSC fill, PPF display transfer and texture copy, the P3D
register file, command-list decoding, vertex arrays and fixed attributes, the
vertex shader (arithmetic opcodes), clipping, strips/fans/lists, three 2D
texture units in every uncompressed format, six TEV stages with the combiner
buffer, alpha test, depth test, blending, logic ops, write masks and scissor.
It follows Citra's software renderer operation by operation (fill rule, 12.4
coordinates, integer blend maths); two deliberate differences: bilinear
filtering when the magnification filter bit is set (Citra's software path is
nearest only), and bounds checks on every buffer.

Whatever it does not implement (geometry shaders, shader flow control,
fragment lighting, fog, stencil, shadow/cube/proc textures, ETC1, mipmaps,
immediate mode) is counted and logged, never silently ignored.

It is used in two places:
- **host**: `make -C tools/ios3ds-pica test` (encoders, tiling, driver core
  and model end to end) and `make -C tools/ios3ds-gl host-test` (gltest on
  both back ends, pixel-compared);
- **QEMU**: `emu/qemu/hw/arm/nintendo3ds.c` maps it at 0x10400000 (below the
  PDC registers), lets it reach FCRAM and VRAM, drives GIC lines 40/41/44/45
  from its status bits, and reports `PDN_GPU_CNT = 0x1007F`.

**What this does and does not prove.** The model was written from the same
references as the encoders, by the same hand. It catches wrong logic in the
back end (state tracking, batching, target/texture management, orientation,
the protocol, the kernel service) and any encoder/decoder disagreement. It
cannot catch a misreading of the references that both sides share, hardware
behaviour the references do not describe (timing, the cache maintenance,
power-up), or registers that must be written and are not. Those are for §8.

## 6. Using it

```
make -C tools/ios3ds-pica test        # unit tests + driver core against the model
make -C tools/ios3ds-gl host-test     # gltest: software vs PICA200 (model), compared
emu/build-qemu.sh                     # QEMU with the GX model (rebuild needed once)
make -C tools/ios3ds-gl && make -C boot && python3 scripts/mksd.py --image build/sd.img
python3 emu/run.py --time 450 --shots 449
mtype -i build/sd.img@@1048576 ::/ios3ds/log.txt | grep -a 'pica\|gltest'
```

`IOS3DS_GL` in a process's environment picks the back end for GL contexts
created afterwards: `pica` or `auto` = GPU if it comes up, anything else or
unset = software. `ios3ds-gltest` renders its scene on a software context,
then creates a second context with `IOS3DS_GL=pica`, renders again, runs the
same probes and compares the two frames pixel by pixel. A second scene, only
compared between the back ends, covers the less common paths: a viewport
hanging over the target, an NPOT texture, two texture units, alpha test,
`GL_COMBINE`, colour and depth masks, a masked scissored clear and
render-to-texture. If a demo time is
given (plist arguments `delay seconds`), the on-screen animation runs on the
GPU context.

Console lines to expect (QEMU, and hopefully hardware):

```
[pica] bring-up: PDN_GPU_CNT 0x0001007f, GPUPROT 0x00000000, GX clock ..., GX stat ...
[pica] arena 8192 KiB at VA 0x... = PA 0x2...; driver ready (state 0)
[gl] pica: GPU up: arena 8192 KiB at PA 0x2... (8064 KiB free), PDN_GPU_CNT 0x1007f, PICA200
[gl] context 0x... uses the ios3ds PICA200
gltest: GL_VERSION "OpenGL ES-CM 1.1 ios3ds", GL_RENDERER "ios3ds PICA200"
gltest: ... ok   (10 probes)
gltest: scene 1, pica vs software: N of 153600 pixels differ at all, M by more than 24 (max ..) ok
gltest: scene 2, pica vs software: ... ok
gltest: PICA200 PASS (0 failed)
```

## 7. Verification so far

Commands and output are in `re-log.md`, entry 24.
- **Host** (`make -C tools/ios3ds-pica test`, `make -C tools/ios3ds-gl host-test`):
  328 unit checks pass. gltest passes all 10 probes on both back ends. Of
  153,600 pixels, 10 differ by more than 24 between the two (triangle edges,
  fill rule); 27,584 differ at all, by rounding (the GPU path interpolates and
  blends in 8-bit fixed point). In scene 2, 320 pixels differ by
  more than 24 (texel boundaries of a nearest-filtered texture).
- **QEMU** (`-M nintendo3ds`, stock iOS 4.2.1 userland): the shim brings the
  service up (`PDN_GPU_CNT 0x0001007f`, 8 MiB arena from
  `IOMallocContiguous`), `ios3ds-gltest` gets `GL_RENDERER "ios3ds PICA200"`,
  all 10 probes pass, the frame comparison gives the same numbers as on the
  host, and the animated scene drawn through the emulated GPU appears on the
  top LCD. The model logged no unsupported operation. The home screen and the
  software context are unaffected.
- **Real hardware**: not run.

## 8. First run on a real 3DS: what to look at

1. Boot as usual; `log.txt` on the SD card (or the top-screen console) has
   the `[pica]` and `gltest:` lines. gltest runs at boot; nothing else uses
   the GPU, so a failure cannot take the home screen down.
2. `[pica] bring-up:` shows `PDN_GPU_CNT`. Expected 0x0001007F. Anything else
   means Luma did not leave the GPU running; the shim tries to start it and
   logs that.
3. `[pica] TIMEOUT waiting for …` names the engine that did not finish
   (first PPF texture copy, P3D, PSC, PPF) and dumps the status registers.
   - P3D timeout on the first list: suspect the configuration-mode bit, the
     list address (GPUPROT, FCRAM reachability) or the end marker.
   - PPF/PSC timeout: suspect the "done" bit positions (they are from
     libn3ds/3dbrew, not measured).
4. `gltest: PICA200 … FAIL` with all probes wrong but no timeout: suspect the
   cache maintenance (stale lines), byte order of the read-back, or row order.
   The probe lines print the RGB they read; compare with the software lines
   just above. A frame that is correct but mirrored vertically means the
   transfer's row order differs from Citra's.
5. Individual probes wrong: texture probe → tiling/filter/texture cache;
   blend probe → blend register; cube probe → depth mapping or culling
   direction; scissored clear → scissor register.
6. To see it on screen, set the gltest plist arguments to e.g. `60 30`.

Then: make `auto` the default, move waits to the P3D interrupt, map the arena
into clients (or at least present with a PPF transfer straight into the
framebuffer), and move T&L into the vertex shader.
