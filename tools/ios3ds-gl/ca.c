/* ios3ds GL engine: CoreAnimation's compositor on the PICA200 (docs/gpu.md 7).
 *
 * SpringBoard (the render server) runs with IOS3DS_CA=1. QuartzCore's display
 * probes for an EAGL context at start-up; if it gets one, CA composites every
 * frame with OpenGL ES through this engine, otherwise in software. So the
 * decision is made once, at that first context:
 *   1. boot.cfg (gpu=, ca_gpu=) through the GPU service (pica_be.c),
 *   2. the PICA200 back end's bring-up and self-check,
 *   3. ca_scene_check() below: a small CA-like scene (clear, alpha-blended
 *      texture, translucent fill, a rotated quad) drawn on the CPU rasterizer
 *      and on the GPU, compared pixel by pixel.
 * Any failure: no context, CA's own software compositor, one log line.
 * After that a GPU failure turns the back end into the CPU rasterizer for
 * the rest of the run (pica_be.c gpu_fail): the screen keeps working.
 *
 * ca_frame() is called on every CA swap and logs, every 30 s, what a frame
 * costs the render thread.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "gl_slots.h"

typedef void (*F_u)(GC *, GLenum);
typedef void (*F_uu)(GC *, GLenum, GLenum);
typedef void (*F_ui)(GC *, GLenum, GLuint);
typedef void (*F_uuu)(GC *, GLenum, GLenum, GLint);
typedef void (*F_gen)(GC *, GLsizei, GLuint *);
typedef void (*F_del)(GC *, GLsizei, const GLuint *);
typedef void (*F_teximg)(GC *, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*F_fbtex)(GC *, GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*F_4i)(GC *, GLint, GLint, GLsizei, GLsizei);
typedef void (*F_v)(GC *);
typedef void (*F_6f)(GC *, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*F_4f)(GC *, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*F_3f)(GC *, GLfloat, GLfloat, GLfloat);
typedef void (*F_ptr)(GC *, GLint, GLenum, GLsizei, const void *);
typedef void (*F_draw)(GC *, GLenum, GLint, GLsizei);
typedef void (*F_read)(GC *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef GLenum (*F_err)(GC *);
#define FN(tab, f, T) ((T)(tab)[SLOT_##f])

enum { S = 96 };                        /* scene size */
#define FBO 0x8D40                      /* GL_FRAMEBUFFER_OES */
#define COL0 0x8CE0                     /* GL_COLOR_ATTACHMENT0_OES */

/* draw the scene through gc's dispatch table into a texture-backed FBO; read it back */
static int scene(GC *gc, void **t, uint8_t *out)
{
    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static uint32_t px[32 * 32];
    static float v[8];
    GLuint tex[2] = { 0, 0 }, fb = 0;
    int x, y;
    for (y = 0; y < 32; y++)                /* RGBA bytes: alpha ramp, colour cells */
        for (x = 0; x < 32; x++)
            px[y * 32 + x] = (uint32_t)(x * 8) << 24 | ((x ^ y) & 4 ? 0x2080F0u : 0xF0C040u);
    FN(t, glGenTextures, F_gen)(gc, 2, tex);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex[0]);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    FN(t, glGenFramebuffersOES, F_gen)(gc, 1, &fb);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, fb);
    FN(t, glFramebufferTexture2DOES, F_fbtex)(gc, FBO, COL0, GL_TEXTURE_2D, tex[0], 0);
    FN(t, glBindTexture, F_ui)(gc, GL_TEXTURE_2D, tex[1]);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    FN(t, glTexParameteri, F_uuu)(gc, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    FN(t, glTexImage2D, F_teximg)(gc, GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FN(t, glViewport, F_4i)(gc, 0, 0, S, S);
    FN(t, glMatrixMode, F_u)(gc, GL_PROJECTION);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glOrthof, F_6f)(gc, 0, S, 0, S, -1, 1);
    FN(t, glMatrixMode, F_u)(gc, GL_MODELVIEW);
    FN(t, glLoadIdentity, F_v)(gc);
    FN(t, glClearColor, F_4f)(gc, 0.1f, 0.2f, 0.3f, 1.0f);
    FN(t, glClear, F_u)(gc, GL_COLOR_BUFFER_BIT);
    FN(t, glEnable, F_u)(gc, GL_BLEND);
    FN(t, glBlendFunc, F_uu)(gc, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);           /* premultiplied, as CA */
    FN(t, glEnableClientState, F_u)(gc, GL_VERTEX_ARRAY);
    FN(t, glEnableClientState, F_u)(gc, GL_TEXTURE_COORD_ARRAY);
    FN(t, glVertexPointer, F_ptr)(gc, 2, GL_FLOAT, 0, v);
    FN(t, glTexCoordPointer, F_ptr)(gc, 2, GL_FLOAT, 0, quad);
    /* a textured, blended quad */
    FN(t, glEnable, F_u)(gc, GL_TEXTURE_2D);
    FN(t, glTexEnvi, F_uuu)(gc, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    FN(t, glColor4f, F_4f)(gc, 1, 1, 1, 1);
    v[0] = 8; v[1] = 8; v[2] = 72; v[3] = 8; v[4] = 8; v[5] = 72; v[6] = 72; v[7] = 72;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    /* a translucent fill, rotated */
    FN(t, glDisable, F_u)(gc, GL_TEXTURE_2D);
    FN(t, glBlendFunc, F_uu)(gc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    FN(t, glColor4f, F_4f)(gc, 0.9f, 0.1f, 0.4f, 0.5f);
    FN(t, glTranslatef, F_3f)(gc, 60, 50, 0);
    FN(t, glRotatef, F_4f)(gc, 30, 0, 0, 1);
    v[0] = -24; v[1] = -16; v[2] = 24; v[3] = -16; v[4] = -24; v[5] = 16; v[6] = 24; v[7] = 16;
    FN(t, glDrawArrays, F_draw)(gc, GL_TRIANGLE_STRIP, 0, 4);
    FN(t, glDisable, F_u)(gc, GL_BLEND);
    FN(t, glDisableClientState, F_u)(gc, GL_TEXTURE_COORD_ARRAY);
    FN(t, glReadPixels, F_read)(gc, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, out);
    FN(t, glBindFramebufferOES, F_ui)(gc, FBO, 0);
    FN(t, glDeleteFramebuffersOES, F_del)(gc, 1, &fb);
    FN(t, glDeleteTextures, F_del)(gc, 2, tex);
    return FN(t, glGetError, F_err)(gc) == GL_NO_ERROR;
}

int ca_scene_check(void)
{
    static void *tab[2][GLI_NSLOTS];
    static uint8_t img[2][S * S * 4];
    const Backend *be[2] = { &sw_backend, &pica_backend };
    int i, ok = 1, edge = 0, max = 0;
    for (i = 0; i < 2 && ok; i++) {
        Share *sh = gl_share_new();
        GC *gc = sh ? gl_gc_new(sh) : NULL;
        if (!gc) {
            ok = 0;
            break;
        }
        gc->be = be[i];
        gl_install_dispatch(gc, tab[i]);
        ok = scene(gc, tab[i], img[i]);
        gl_gc_free(gc);
        gl_share_release(sh);
    }
    if (!ok) {
        gl_log("CA scene check: GL error while drawing");
        return 0;
    }
    for (i = 0; i < S * S; i++) {
        int k, d = 0;
        for (k = 0; k < 4; k++) {
            int e = abs(img[0][i * 4 + k] - img[1][i * 4 + k]);
            d = e > d ? e : d;
        }
        max = d > max ? d : max;
        if (d > 24)
            edge++;                     /* polygon edges: the GPU's fill rule differs slightly */
    }
    gl_log("CA scene check: GPU vs CPU rasterizer: %d of %d pixels differ by more than 24 (max %d)%s",
           edge, S * S, max, edge > S * S / 50 ? ": too many" : "");
    return edge <= S * S / 50;
}

/* ------------------------------------------------------------------ */
/* per-frame cost of the render thread (log every 30 s)                */
/* ------------------------------------------------------------------ */

#ifndef GL_HOST
typedef unsigned int mach_port_t;
extern mach_port_t mach_thread_self(void);
extern int thread_info(mach_port_t, int, int *, unsigned *);
extern int mach_port_deallocate(mach_port_t, mach_port_t);
extern mach_port_t mach_task_self_;
static uint64_t thread_cpu_us(void)
{
    struct { int us, uus, ss, sus, rest[6]; } bi;
    unsigned n = 10;
    mach_port_t t = mach_thread_self();
    uint64_t r = 0;
    if (thread_info(t, 3 /* THREAD_BASIC_INFO */, (int *)&bi, &n) == 0)
        r = (uint64_t)(bi.us + bi.ss) * 1000000u + (uint64_t)(bi.uus + bi.sus);
    mach_port_deallocate(mach_task_self_, t);
    return r;
}
#else
static uint64_t thread_cpu_us(void) { return 0; }
#endif

/* every CA swap (iface.c SwapNotification); a line every 300 frames */
void ca_frame(void)
{
    static uint64_t cpu0, frames;
    static PicaStats s0;
    PicaStats s;
    uint64_t cpu;
    if (!getenv("IOS3DS_CA"))
        return;
    cpu = thread_cpu_us();
    if (!frames++) {
        cpu0 = cpu;
        pica_backend_stats2(&s0);
        return;
    }
    if (frames % 300 != 1)
        return;
    pica_backend_stats2(&s);
    gl_log("CA: %llu frames; per frame over the last 300: render thread CPU %llu us, %u.%u command lists, "
           "%u.%u GPU presents, %u.%u CPU read-backs (%u KiB), %u.%u texture uploads (%u KiB)",
           (unsigned long long)frames - 1,
           (unsigned long long)((cpu - cpu0) / 300), (s.lists - s0.lists) / 300, (s.lists - s0.lists) * 10 / 300 % 10,
           (s.presents - s0.presents) / 300, (s.presents - s0.presents) * 10 / 300 % 10,
           (s.readbacks - s0.readbacks) / 300, (s.readbacks - s0.readbacks) * 10 / 300 % 10,
           (s.readback_kb - s0.readback_kb) / 300, (s.uploads - s0.uploads) / 300,
           (s.uploads - s0.uploads) * 10 / 300 % 10, (s.upload_kb - s0.upload_kb) / 300);
    cpu0 = cpu;
    s0 = s;
}
