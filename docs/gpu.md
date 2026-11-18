# GPU: OpenGL ES 1.1 on ios3ds

iOS 4.2.1 on the iPod touch 2G draws GL through the MBX Lite GPU. The 3DS has a
PICA200 instead, and Apple's `MBXGLEngine` can't drive it. `tools/ios3ds-gl` is a
replacement engine that plugs into the same slot inside `OpenGLES.framework`.
It draws with a CPU rasterizer by default. A second back end renders on the
PICA200 itself ([pica200.md](pica200.md)); it is opt-in until it has been
verified on real hardware.

Apps, and later QuartzCore, keep using Apple's `OpenGLES.framework` unmodified.
They call `gl*` and `EAGLContext` as usual. Only the engine behind those calls is
ours.

## 1. How OpenGLES.framework finds an engine (8C148)

`_eagl_init` (cache VA 0x34ff870c) runs once, from the first
`-[EAGLSharegroup initWithAPI:]`:

1. It tries `OpenGLES.framework/GLEngine.bundle/GLEngine`, the SGX/GLI engine.
   That bundle doesn't exist on the iPod, so the call fails.
2. `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))` must
   return at least one service. Otherwise EAGL has no engine, and
   `initWithAPI:` returns nil.
3. `CFBundleCopyResourceURL(com.apple.opengles, "MBXGLEngine", "bundle")` and
   `CFBundleCreate` run. Then `CFBundleGetDataPointerForName("GLESGetEGLInterface")`
   is called, and the result is saved in `_mbx_dispatch` (0x3e7d84c8).

Then:
- `-[EAGLSharegroup initWithAPI:]` uses the MBX engine only for `api == 1`
  (ES1). It calls `CreateSharegroup(&S[0x44])` and sets the engine id `S+4 = 2`.
- `-[EAGLContext initWithAPI:properties:]` calls
  `CreateGC(sg, X+0x10, X+0xD34, X+0xC)`. Here `X` is the context's private block:
  - +0xC holds the engine GC;
  - +0x10 and +0xD34 are two 841-slot `__GLIFunctionDispatchRec` tables. The second
    starts at 0x10 + 841·4.

Each exported `gl*` is a trampoline:
`mrc p15,0,rT,c13,c0,3; ldr rX,[rT,#0x78]` (the current context's X), then
`r0 = [X,#0xC]`, `fn = [X,#0x10+4·slot]`, followed by a tail call with the GL
arguments shifted up by one. `tools/ios3ds-gl/genslots.py` reads each export's
slot from your own dyld cache and writes `gl_slots.h`. That file holds only names
and numbers.

### What ios3ds changes (rootfs/recipe.json)

| change | why |
|---|---|
| file patch, cache file offset 0x4ff9458 (OpenGLES `__cstring`, VA 0x34ff9458): `"AppleMBXDevice\0"` → `"IOResources\0\0\0\0"` | Step 2 now matches the always-registered `IOResources` nub. No kernel or device-tree change is needed. Only OpenGLES's copy of the string changes; the copies in MBX2D and MBXGLEngine are untouched. |
| write `/System/Library/Caches/com.apple.dyld/enable-dylibs-to-override-cache` | `MBXGLEngine` is in the shared cache. With this switch, dyld (`sDylibsOverrideCache`) loads the on-disk file at the same path instead. The rootfs keeps no other cached dylibs on disk (only 12 symlinks into frameworks that exist only in the cache), so nothing else changes. |
| file patch, cache file offset 0x46de6a8 (QuartzCore `CA::WindowServer::M2Display::M2Display` literal pool, VA 0x346de6a8): `'L565'` → `'BGRA'` | The display constructor probes `eagl_context(2)`, then `eagl_context(1)`; on success it stores the API at `+0x17c` and the display surface format at `+0x180`: `'BGRA'` for API 2, `'L565'` (16 bpp) for API 1. With this patch an ES1 compositor renders the 32 bpp surfaces the CLCD scans out and the screen mirror reads. (Until 2026-11-18 a patch at 0x46de620 skipped the ES1 probe instead, keeping CA's software compositor; §7.) |
| add `OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine` (0755) | Our engine, ad-hoc signed. The bundle's stock `Info.plist` already names `MBXGLEngine` as its executable. |
| add `/usr/libexec/ios3ds-gltest` and `com.ios3ds.gltest.plist` | The test program (§5). |

A kernel-side alternative to the string patch is to publish an IOService of
class `AppleMBXDevice` from the shim, or to keep the `/arm-io/mbx` device-tree
node with a stub driver. Neither is needed while userland is the only consumer.

## 2. The engine interface

`GLESGetEGLInterface()` returns a table of 11 function pointers. The 4.2.1 table
is the 3.x table of nine plus Set/GetProperty. Every caller was checked in
8C148's OpenGLES; the stock MBXGLEngine's `_GLESBindView`, `_GLESPresentView` and
`_GLESCreateBuffer` confirmed the drawable protocol.

| off | entry | called from | ios3ds |
|---|---|---|---|
| 0x00 | `CreateSharegroup(void **sg)` → ≠0 ok | `-[EAGLSharegroup initWithAPI:]` | `gl_share_new` |
| 0x04 | `DestroySharegroup(sg)` | `-[EAGLSharegroup dealloc]` | refcounted release |
| 0x08 | `CreateGC(sg, front, back, &gc)` → ≠0 ok | `-[EAGLContext initWithAPI:properties:]` | new GC; fills both tables |
| 0x0C | `DestroyGC(gc)` | `-[EAGLContext dealloc]` | |
| 0x10 | `BindCoreSurface(gc, target, IOSurfaceRef)` | `attachImage:toCoreSurface:invertedRender:` (OpenGLES allows `GL_RENDERBUFFER` only with invertedRender=YES, and 0x84F5 only with NO) | renderbuffer: draw straight into the surface. 0x84F5 or `GL_TEXTURE_2D`: the bound texture samples the surface. 32 bpp `'BGRA'` surfaces only. |
| 0x14 | `BindView(gc, drawable, ifmt, retained)` | `renderbufferStorage:fromDrawable:` (`[layer nativeWindow]`; ifmt `GL_RGBA8_OES`/`GL_RGB565_OES`) | see below |
| 0x18 | `FinishTexture` | never called | returns 1 |
| 0x1C | `PresentView(gc)` | `presentRenderbuffer:` | `drawable->present(d, 1)` |
| 0x20 | `SwapNotification(gc, fb_id, transaction, layer)` | `swapNotification:forTransaction:onLayer:` (CA's GL renderer) | `IOMobileFramebufferSwapSignal(main display, transaction, layer)`. 4.2.1's `IOMobileFramebufferGetID` is no longer a port, so a 3.x-style signal by id would never complete CA's swaps. |
| 0x24 | `SetProperty(gc, pname, const void *)` | `setParameter:to:`, profiler (pname 100) | ignored |
| 0x28 | `GetProperty(gc, pname, void *)` | `getParameter:to:`, profiler | writes 0 |

**QuartzCore drawable (`[CAEAGLLayer nativeWindow]`).** This is a block of function
pointers, each called with the drawable as its first argument:

| off | call | stock engine use |
|---|---|---|
| +0x04 | `bind(d, fourcc, CallbackBlock *)` → ≠0 ok | BindView: fourcc `'BGRA'` (0x42475241) for RGBA8/RGB8, `'L565'` for RGB565 |
| +0x08 | `unbind(d)` | rebinding, or failure |
| +0x0C | `dequeue(d)` → IOSurfaceRef | before the first draw of each frame (`_ViewTextureBeginIfNeeded`) |
| +0x10 | `present(d, 1)` → ≠0 ok | PresentView, only if a frame was begun |

`CallbackBlock {ctx, create(ctx, IOSurfaceRef) → 1 ok, destroy(ctx, IOSurfaceRef)}`
is how CA announces each buffer it allocates for the layer. ios3ds always asks for
`'BGRA'`, because the rasterizer draws 0xAARRGGBB words. RGB565/RGB8
renderbuffers keep alpha at 1 instead (`RT.opaque`). The drawable dequeues at
bind time, so `GL_RENDERBUFFER_WIDTH/HEIGHT` are valid straight after
`renderbufferStorage:fromDrawable:`. GL row 0 is the surface's last row. With a
retained backing, the last presented frame is copied into a newly dequeued
buffer.

## 3. Engine layout (`tools/ios3ds-gl`)

| file | role |
|---|---|
| `api.c` | The ES 1.1 state machine: 177 entry points by slot (`gl_install_dispatch`), objects (textures, buffers, renderbuffers, framebuffers, fences), matrices, `glGet*`, pixel transfer. |
| `draw.c` | Front end: vertex fetch (all array types, VBOs, indices), modelview/projection and texture matrices, 8 lights, colour material, fog, primitive assembly. Points and lines become quads. `glDrawTex*OES` is handled here too. Its output is **clip-space triangles** plus the GC state. |
| `pica_be.c` | The PICA200 `Backend` ([pica200.md](pica200.md)): command lists, GPU-side render targets and textures, read-back. |
| `raster.c` | The software `Backend`: clipping, viewport, face culling, perspective-correct spans, 2 texture units (nearest/linear, repeat/clamp/mirror; full `GL_COMBINE`), alpha test, depth test (16-bit), blending (all factors and equations), colour mask, scissor, fog, polygon offset. |
| `iface.c` | EAGL glue: the 11-entry table, IOSurface-backed renderbuffers and textures, the CA drawable protocol. IOSurface and IOMobileFramebuffer are resolved with `dlsym`. |
| `thunks.S` | 841 12-byte stubs. Each unimplemented slot logs `[gl] unimplemented GL call: slot N` to the console once, then returns 0. |
| `genslots.py` → `gl_slots.h` | Maps API names to slot numbers, read from your dyld cache. |
| `test/` | `gltest` (§5) and the host harness. |

**API coverage.** All 44 `gl*` functions QuartzCore imports are implemented
(checked against QuartzCore's import list). So is the rest of ES 1.1 common
profile, plus `OES_framebuffer_object`, `OES_draw_texture`, `OES_point_size_array`, `OES_mapbuffer`, blend equation/func separate,
`APPLE_fence`, the `APPLE` multisample entry points (single-sampled) and
`EXT_discard_framebuffer` (a no-op). Paletted compressed textures are decoded.

**Not done:**
- PVRTC compressed textures (`glCompressedTexImage2D` logs and fails);
- mipmap levels > 0: minification samples the base level;
- stencil: the state is kept, but there is no stencil buffer;
- user clip planes (the state is kept, but nothing is clipped);
- logic op, multisampling, smooth points and lines;
- `matrix_palette` and `weight_pointer`.

## 4. Back ends

The rasterizer sits behind `struct Backend` (`engine.h`):

```c
typedef struct Backend {
    const char *name;
    void (*clear)(GC *, RT *, GLbitfield mask);             /* honours scissor and masks */
    void (*triangles)(GC *, RT *, const Vtx *v, int n);     /* n/3 clip-space triangles */
    void (*tex_changed)(GC *, Tex *);                        /* optional: texture image changed */
    void (*tex_deleted)(Tex *);                              /* optional */
    void (*finish)(GC *, RT *);                              /* optional: make rt->color current */
} Backend;
```

- `Vtx` carries the lit colour, texture coordinates after the texture matrix,
  and the fog factor.
- Every per-fragment setting is in the GC: blend, alpha and depth test, texenv
  and combiners, scissor, cull, colour mask, and polygon offset.
- `RT` names the target: its colour words, a 16-bit depth buffer, the pitch, and
  `flip` (GL row 0 is the last memory row).
- `Tex.be_priv`/`be_seq` let a back end cache its own copy of a texture.
- `gl_be_forget(mem)` tells back ends that renderbuffer or texture storage is
  about to be freed.

There are two:

| back end | file | `GL_RENDERER` |
|---|---|---|
| CPU rasterizer (default, and the fallback) | `raster.c` | `ios3ds software rasterizer` |
| PICA200 GPU | `pica_be.c` + `tools/ios3ds-pica` | `ios3ds PICA200` |

`gl_gc_new` asks `gl_pick_backend()`: with `IOS3DS_GL=pica` (or `auto`) in the
process environment and a usable GPU the context draws on the PICA200,
otherwise on the CPU. The GPU back end, the kernel service behind it and the
software GPU model used to test it are described in [pica200.md](pica200.md).

## 5. Test: `ios3ds-gltest`

The test is `tools/ios3ds-gl/test/gltest.c`. It uses only the public API: an
`EAGLContext` through the ObjC runtime, plus `gl*` from OpenGLES.framework.

1. It renders the test scene into an offscreen FBO (RGBA8 colour + DEPTH16). The
   scene has:
   - a smooth triangle;
   - a GL_LINEAR checkerboard texture;
   - `glDrawTexiOES`;
   - a 50 % blended quad;
   - a scissored clear;
   - a lit, culled, depth-tested cube in a frustum.
2. It reads back 10 probe pixels and prints `ok`/`FAIL` for each, then
   `PASS`/`FAIL`, to the console.
   It then repeats this on a second context created with `IOS3DS_GL=pica` and
   compares the two frames pixel by pixel (skipped, with a log line, when the
   GPU is not usable).
3. After `delay` seconds it binds the main display's framebuffer IOSurface
   (`IOMobileFramebufferGetLayerDefaultSurface`) as a renderbuffer with
   `attachImage:toCoreSurface:invertedRender:YES`. For `seconds` it then draws the
   animated scene straight onto the screen, which the ARM9 mirrors to the top
   LCD.

The launch daemon runs it with `360 60`: the probe test runs at boot, and the on-screen demo starts at about 425 s guest time, after the home screen is up. To keep only the boot self-test, set both arguments to `0`.

To build and run:

```
make -C tools/ios3ds-gl               # engine bundle + gltest into build/rootfs-add
make -C tools/ios3ds-gl host-test     # same core + scene on the build host (offscreen, PPM)
make -C boot && python3 scripts/mksd.py --image build/sd.img
python3 emu/run.py --time 450 --shots 415,449     # home screen, then the GL scene
mtype -i build/sd.img@@1048576 ::/ios3ds/log.txt | grep -a 'gl'
```

## 6. Status

Verified in QEMU (`-M nintendo3ds`, 450 s boot, gltest args `360 60`):
- `[gl] context created (software rasterizer) (pid 8)`: OpenGLES loads our bundle from disk past the cache copy, and `CreateGC` fills the dispatch tables.
- `gltest: PASS (0 failed)`: the 10 probes and `glGetError` match the host build value for value. This covers smooth shading, a LINEAR texture, DrawTex, blending, scissor, and depth/lighting/culling.
- `[gl] first BindCoreSurface`, then `drawing into the display surface 320x480`: from about 425 s the top LCD shows the animated scene, rotated 90° like everything else on the portrait display. It runs at ~13 fps including a 50 ms sleep per frame, i.e. about 25 ms per 320x480 frame in the emulator.
- In the same boot, the 415 s screenshot shows the SpringBoard home screen as before. With the QuartzCore probe patch, SpringBoard no longer creates a GL context. Without the patch the screen stayed black (§1).
- No `unimplemented GL call` was logged.
- `make host-test` gives the same pixels on the build host.

Screenshots aren't committed (iOS artwork); rerun the commands above to reproduce them.

**Verified only by RE, not exercised yet:** `BindView`/`PresentView` (the CAEAGLLayer path of a UIKit GL app). Texture-from-IOSurface and `SwapNotification` now run in CA's compositor (§7).

**Next:**
- CA compositor without the per-frame read-back of the display surface (§7);
- a UIKit GL app through CAEAGLLayer;
- PVRTC decode and mipmaps;
- the PICA200 back end on real hardware ([pica200.md](pica200.md) §8).

## 7. CoreAnimation's compositor on the engine (and on the PICA200)

SpringBoard is the render server: QuartzCore's `CA::WindowServer` composites
every app's layers into the display surface. Without an EAGL engine it does
that in software on the ARM11; with one it uses `CA::OGL::GLESContext`, i.e.
OpenGL ES 1.1 through our engine, and with the PICA back end the per-pixel
work (blending, texture sampling, transforms) moves to the GPU.

### How CA drives the engine (RE of 8C148's QuartzCore)

- **Display probe** (`M2Display::M2Display`, 0x346de5b8): `eagl_context(2)`,
  then `eagl_context(1)` (= `-[EAGLContext initWithAPI:]`, i.e. our
  `CreateSharegroup`/`CreateGC`). The first that succeeds selects GL
  compositing; API 1 picks the surface format `'L565'`, patched to `'BGRA'`.
- **Display surface as render target** (`GLESContext::make_buffer_current`,
  0x3470d274): `glGenTextures`, `glBindTexture(0x84F5, …)` (0x84F5 =
  GL_TEXTURE_RECTANGLE), NEAREST/CLAMP parameters, then
  `-[EAGLContext attachImage:toCoreSurface:invertedRender:NO]` with target
  0x84F5 = our `BindCoreSurface` (the texture's pixels are the IOSurface),
  then an FBO with that texture as `GL_COLOR_ATTACHMENT0`.
- **GL usage**: the 44 imported `gl*` functions (§3), all implemented. Per
  call site (disassembly of the whole `__text`, imports resolved through
  `__picsymbolstub4` + the indirect symbol table): textures bound with a
  per-image target (2D or 0x84F5), `glTexEnvfv(GL_TEXTURE_ENV_COLOR)` and
  `GL_COMBINE` tables, `glTexImage2D`/`SubImage2D` uploads,
  `glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE)` (only with
  APPLE_client_storage, not advertised), `glGetString(0x8A1E)` after
  `GL_EXTENSIONS` (an Apple-private second list), `glGetIntegerv(0x84F8)`
  (max rectangle texture size), and `glFlush` with FBO 0 bound before the swap.
- **Present**: `SwapNotification` → `IOMobileFramebufferSwapSignal`.

### What the engine needed

| first run's symptom | fix |
|---|---|
| `glBindTexture(0x84F5)` → GL_INVALID_ENUM | 0x84F5 accepted wherever GL_TEXTURE_2D is (bind, parameters, images, enable); a texture last bound as 0x84F5 takes texel coordinates (host test: same pixels as 2D with 0..1) |
| `[gl] Get: unknown pname 0x84f8` | max rectangle size = 1024 |
| `GL error 0x500` in `GetString` → `CoreAnimation: rendering error 500` | `glGetString(0x8A1E)` returns "" |
| screen black except for one icon | `glFlush` did nothing and `glFinish` only synced the *bound* framebuffer; CA flushes with FBO 0 bound. Now `glFlush`/`glFinish`/`SwapNotification` write every GPU target back (`Backend.flush_all`) |

`IOS3DS_GL_TRACE=1` (set for SpringBoard) logs the first GL error of every
call site as `[gl] trace: GL error 0x… at engine+0x…` (look it up with `nm`).

### Switching it on, and falling back

SpringBoard's environment (rootfs/recipe.json) has `CA_ENABLE_OGL=1` and
`IOS3DS_CA=1`. In a process with `IOS3DS_CA`, the first `CreateGC` decides
once (`tools/ios3ds-gl/ca.c`, `pica_ca_gate`):
1. boot.cfg `ca_gpu` (and `gpu`) through the GPU service (`PICA_OP_CONFIG`);
2. the PICA200 back end's bring-up and self-check;
3. a CA-like scene (clear, premultiplied textured quad, rotated translucent
   fill) drawn on the CPU rasterizer and on the GPU and compared (≤ 2 % of
   pixels may differ by more than 24, polygon edges).

If any step fails the context is refused: QuartzCore's probe fails and CA
composites in software exactly as before (one `[gl] CA compositor: software
(…)` line). CA on our *CPU* rasterizer is never used, it would be slower than
CA's own software path. A GPU failure later in the run turns the back end
into the CPU rasterizer for the rest of the run (`[gl] pica: … GPU rendering
is OFF`): the screen keeps working, slower. Kill switch: boot.cfg `ca_gpu=0`
(or `gpu=0`).

### Results (QEMU, re-log 31)

- Home screen, Spotlight + keyboard, page swipes, Settings launch and exit
  render through the PICA200 model; screenshots against the software
  compositor at the same moments: 2181 of 192000 pixels differ, by at most 1
  (rounding), on still frames; animation frames differ where the two runs
  are at different animation times.
- Cost: see re-log 31. In QEMU the GPU path is not cheaper for SpringBoard,
  because every frame writes the whole 320x480 display surface back through
  the arena (PPF tiled→linear, copy-out, a per-pixel byte rotation), and
  QEMU runs CA's software compositor at host speed. The engine logs
  `[gl] CA: … per frame: render thread CPU … us, … read-backs (… KiB)` every
  300 frames, so a hardware run measures it directly.
- Next: present without the CPU read-back (render the final frame through a
  GPU swizzle pass and PPF it straight into the display surface's physical
  memory, which is VRAM), and read back only the rows CA changed.
