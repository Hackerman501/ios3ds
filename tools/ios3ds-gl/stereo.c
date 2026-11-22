/* ios3ds GL engine: the stereoscopic 3D stamp (docs/stereo3d.md).
 *
 * SpringBoard's CoreAnimation composites every frame through this engine
 * (ca.c). With boot.cfg stereo3d=1 (the ARM9 appends ios3ds_stereo=1 to the
 * kernel boot-args) each draw into the display target is classified by the
 * shared layer classifier (boot/common/stereo.h) and, when the frame turns
 * out to be the home screen, its depth class is stamped into the alpha byte
 * of the pixels it covers with a second, alpha-only pass over the same
 * geometry. The ARM9 screen copy reads that class map back out of the
 * composed frame and builds the left and right eye images from it.
 *
 * Pass 1 is always CoreAnimation's own draw, untouched: colours, blending
 * and coverage are exactly what they would be without stereo. The stamp
 * pass writes only the alpha channel (colour mask A, no blend, no texture),
 * so it cannot disturb the picture.
 *
 * Frames that are not the home screen (apps, the lock screen, Spotlight,
 * a frame CA only partly redraws into a stale class map) are wiped flat at
 * the end of the frame: one full-screen quad that sets alpha to SA_FLAT
 * everywhere, so the screen copy sees a uniform frame and gives both eyes
 * the same image.
 *
 * Only the render server (IOS3DS_CA=1, SpringBoard) ever stamps: without
 * that environment variable the flag is ignored, so gltest and apps are
 * unaffected. One thread composites; the state here is not locked.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef GL_HOST
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif
#include "engine.h"
#include "stereo.h"

static void stereo_wipe(GC *gc, const RT *rt);

static int st_state;                /* 0 = not tried, 1 = off, 2 = on */
static stereo_frame_t st_f;
static RT st_rt;                    /* the display target of this frame */
static int st_have_rt;
static unsigned st_home, st_flat, st_wipes, st_stamps, st_frames;
static int st_log;                  /* diag=1: log the frame structure */
/* the first draws of the frame, for the log */
#define ST_LOG_MAX 12
static struct { int16_t x0, y0, x1, y1; int8_t cls; } st_log_draws[ST_LOG_MAX];
static int st_log_n;
static uint32_t st_log_sig, st_log_last;

static int bootargs_has(const char *key)
{
#ifdef GL_HOST
    const char *e = getenv("IOS3DS_STEREO");
    (void)key;
    return e && *e && *e != '0';
#else
    char ba[256];
    size_t n = sizeof ba - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0)
        return 0;
    ba[n] = 0;
    return strstr(ba, key) != NULL;
#endif
}

void stereo_init(void)
{
    if (st_state)
        return;
    st_state = 1;
    if (!getenv("IOS3DS_CA"))
        return;                     /* only the CoreAnimation render server */
    if (!bootargs_has("ios3ds_stereo=1"))
        return;
    st_state = 2;
    st_log = bootargs_has("ios3ds_diag=1");
    stereo_frame_begin(&st_f);
    gl_log("stereo3D: on (boot-arg ios3ds_stereo=1): depth classes are stamped into the frame's alpha%s",
           st_log ? ", frame structure logged" : "");
}

int stereo_enabled(void)
{
    return st_state == 2;
}

/* Classify one draw into the display target. Returns the depth class to
   stamp (>= 0) or -1; remembers the target for the end-of-frame wipe. */
int stereo_prepare(GC *gc, RT *rt, const Vtx *v, int n)
{
    float minx, miny, maxx, maxy, x, y, w;
    int i, full, opaque, cls;
    float tol = 4.0f;

    if (!st_state)
        stereo_init();
    if (st_state != 2 || !rt->surface || n < 3 || !gc->vp[2] || !gc->vp[3])
        return -1;
    minx = miny = 1e30f;
    maxx = maxy = -1e30f;
    for (i = 0; i < n; i++) {
        w = v[i].w != 0.0f ? v[i].w : 1.0f;
        x = (v[i].x / w * 0.5f + 0.5f) * (float)gc->vp[2] + (float)gc->vp[0];
        y = (v[i].y / w * 0.5f + 0.5f) * (float)gc->vp[3] + (float)gc->vp[1];
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    full = minx <= (float)gc->vp[0] + tol && maxx >= (float)(gc->vp[0] + gc->vp[2]) - tol &&
           miny <= (float)gc->vp[1] + tol && maxy >= (float)(gc->vp[1] + gc->vp[3]) - tol;
    opaque = 1;
    for (i = 0; i < n && opaque; i++)
        if (v[i].c[3] < 0.999f)
            opaque = 0;
    if (opaque) {
        int u;
        for (u = 0; u < MAX_TEX_UNITS; u++) {
            Tex *t;
            if (!gc->tu[u].enabled)
                continue;
            t = gl_bound_tex(gc, u);
            if (!t || !t->w || !t->h || !stereo_tex_opaque(t)) {
                opaque = 0;
                break;
            }
        }
    }
    cls = stereo_draw(&st_f, (int)minx, (int)miny, (int)maxx, (int)maxy, gc->vp, full, opaque);
    st_rt = *rt;
    st_have_rt = 1;
    if (cls >= 0) {
        st_stamps++;
        if (st_log && st_log_n < ST_LOG_MAX) {
            st_log_draws[st_log_n].x0 = (int16_t)minx;
            st_log_draws[st_log_n].y0 = (int16_t)miny;
            st_log_draws[st_log_n].x1 = (int16_t)maxx;
            st_log_draws[st_log_n].y1 = (int16_t)maxy;
            st_log_draws[st_log_n].cls = (int8_t)cls;
            st_log_n++;
        }
    }
    return cls;
}

/* Is this texture's image opaque (any texel with alpha < 255 = not)?
   Cached in Tex.stereo_opq (0 = unknown): the pixels are scanned once per
   image and again whenever the image changes (gl_tex_dirty). */
int stereo_tex_opaque(Tex *t)
{
    int x, y;
    const uint32_t *p;
    if (t->stereo_opq)
        return t->stereo_opq == 1;
    if (!t->px || !t->w || !t->h || t->w > MAX_TEX_SIZE || t->h > MAX_TEX_SIZE) {
        t->stereo_opq = 1;          /* no pixels to inspect: assume opaque */
        return 1;
    }
    p = t->px;
    for (y = 0; y < t->h; y++) {
        for (x = 0; x < t->w; x++)
            if ((p[y * t->pitch + x] >> 24) != 0xFF) {
                t->stereo_opq = 2;
                return 0;
            }
    }
    t->stereo_opq = 1;
    return 1;
}

/* Pass 1 is CoreAnimation's draw; pass 2 stamps the class into the alpha
   channel of the same pixels (colour mask A only, blending off, no
   texture: the fragment alpha is the vertex alpha we set). */
void gl_be_triangles(GC *gc, RT *rt, const Vtx *v, int n, int cls)
{
    int i;
    int cm[4], blend, depth_test, u0, u1;
    float a;
    Vtx *w = (Vtx *)v;

    gc->be->triangles(gc, rt, v, n);
    /* The stamp pass keeps pass 1's cull, scissor, depth and alpha-test
       behaviour so both passes cover the same pixels; only a draw without
       an alpha test can be stamped safely. */
    if (st_state != 2 || cls < 0 || n < 3 || gc->alpha_test)
        return;
    a = (float)stereo_magic(cls) / 255.0f;
    memcpy(cm, gc->color_mask, sizeof cm);
    blend = gc->blend;
    depth_test = gc->depth_test;
    u0 = gc->tu[0].enabled; u1 = gc->tu[1].enabled;
    for (i = 0; i < n; i++)
        w[i].c[3] = a;
    gc->color_mask[0] = gc->color_mask[1] = gc->color_mask[2] = 0;
    gc->color_mask[3] = 1;
    gc->blend = 0;
    gc->depth_test = 0;
    gc->tu[0].enabled = 0;
    gc->tu[1].enabled = 0;
    gc->be->triangles(gc, rt, w, n);
    memcpy(gc->color_mask, cm, sizeof cm);
    gc->blend = blend;
    gc->depth_test = depth_test;
    gc->tu[0].enabled = u0; gc->tu[1].enabled = u1;
}

/* End of frame (glFlush / SwapNotification): decide whether the class map
   stays (the home screen) or is wiped flat, and start the next frame.
   Returns 1 if a wipe was drawn, so the caller syncs the target. */
int stereo_end_frame(GC *gc)
{
    int home, i;
    uint32_t sig = 0;

    if (st_state != 2 || !st_f.draws || !st_have_rt)
        return 0;
    home = stereo_frame_home(&st_f);
    st_frames++;
    if (home) {
        st_home++;
    } else {
        st_flat++;
        st_wipes++;
        stereo_wipe(gc, &st_rt);
    }
    if (st_log) {
        for (i = 0; i < st_log_n; i++)
            sig = sig * 31u + (uint32_t)(st_log_draws[i].cls + 1) * 4096u +
                  (uint32_t)(st_log_draws[i].y0 & 1023) * 4u + (st_log_draws[i].x0 & 3);
        sig = sig * 31u + (uint32_t)st_f.phase * 8u + st_f.icons;
        if (sig != st_log_last || (st_frames % 60) == 0) {
            char b[200];
            int n = 0;
            st_log_last = sig;
            n = snprintf(b, sizeof b, "stereo frame %u: %u draws, phase %d, icons %d -> %s [",
                         st_frames, st_f.draws, st_f.phase, st_f.icons, home ? "home" : "flat");
            for (i = 0; i < st_log_n && n < (int)sizeof b - 40; i++)
                n += snprintf(b + n, sizeof b - n, "%s%d:(%d,%d-%d,%d)",
                              i ? " " : "", st_log_draws[i].cls, st_log_draws[i].x0,
                              st_log_draws[i].y0, st_log_draws[i].x1, st_log_draws[i].y1);
            snprintf(b + n, sizeof b - n, "]");
            gl_log("%s", b);
        }
    }
    stereo_frame_begin(&st_f);
    st_log_n = 0;
    st_have_rt = 0;
    return home ? 0 : 1;
}

/* Wipe the class map of a frame that is not the home screen: one
   full-screen quad that writes SA_FLAT into the alpha channel only. */
static void stereo_wipe(GC *gc, const RT *rt)
{
    static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    Vtx q[6];
    int i, cm[4], blend, alpha_test, depth_test, cull, scissor_on, vp[4];
    int u0, u1;

    memcpy(cm, gc->color_mask, sizeof cm);
    blend = gc->blend; alpha_test = gc->alpha_test; depth_test = gc->depth_test;
    cull = gc->cull; scissor_on = gc->scissor_on;
    vp[0] = gc->vp[0]; vp[1] = gc->vp[1]; vp[2] = gc->vp[2]; vp[3] = gc->vp[3];
    u0 = gc->tu[0].enabled; u1 = gc->tu[1].enabled;
    memset(q, 0, sizeof q);
    for (i = 0; i < 6; i++) {
        q[i].x = corner[i][0];
        q[i].y = corner[i][1];
        q[i].w = 1;
        q[i].fog = 1;
        q[i].c[3] = 0;                      /* SA_FLAT */
    }
    gc->vp[0] = 0;
    gc->vp[1] = 0;
    gc->vp[2] = rt->w;
    gc->vp[3] = rt->h;
    gc->color_mask[0] = gc->color_mask[1] = gc->color_mask[2] = 0;
    gc->color_mask[3] = 1;
    gc->blend = 0;
    gc->alpha_test = 0;
    gc->depth_test = 0;
    gc->cull = 0;
    gc->scissor_on = 0;
    gc->tu[0].enabled = 0;
    gc->tu[1].enabled = 0;
    gc->be->triangles(gc, (RT *)rt, q, 6);
    memcpy(gc->color_mask, cm, sizeof cm);
    gc->blend = blend; gc->alpha_test = alpha_test; gc->depth_test = depth_test;
    gc->cull = cull; gc->scissor_on = scissor_on;
    gc->vp[0] = vp[0]; gc->vp[1] = vp[1]; gc->vp[2] = vp[2]; gc->vp[3] = vp[3];
    gc->tu[0].enabled = u0; gc->tu[1].enabled = u1;
}

void stereo_stats(unsigned *frames, unsigned *home, unsigned *flat, unsigned *stamps, unsigned *wipes)
{
    if (frames) *frames = st_frames;
    if (home) *home = st_home;
    if (flat) *flat = st_flat;
    if (stamps) *stamps = st_stamps;
    if (wipes) *wipes = st_wipes;
}
