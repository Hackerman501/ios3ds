/* ios3ds GL engine: the PICA200 Backend (docs/pica200.md).
 *
 * The front end (draw.c) hands over lit clip-space triangles; this file
 * turns them, plus the GC's raster state, into PICA200 command lists:
 *
 *   Vtx array  -> vertex buffer in the GPU arena (the struct is the vertex format)
 *   state      -> viewport/scissor/depth/blend/alpha registers, TEV stages
 *   textures   -> 8x8-tiled RGBA8 copies in the arena (Tex.be_priv)
 *   RT         -> a tiled colour + D24S8 depth buffer in the arena per render
 *                 target; rt->color (linear BGRA: IOSurfaces, renderbuffers,
 *                 texture images) is brought in with a display transfer when
 *                 the GPU copy is stale and written back by finish()
 *
 * Draws are batched into one command list per flush. The GPU is reached
 * through pica_dev_t: the ios3ds_gpu sysctl on iOS, the software model
 * in the host build. Anything this back end cannot do falls back to the
 * software rasterizer for that render target, with a log line.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "pica.h"
#include "pica_dev.h"

#define MAX_TARGETS 4
#define MAX_DEPTHS  4
#define VTX_CAP     6144                /* vertices per command list */
#define CMD_WORDS   24576               /* 96 KiB, below the driver's staging buffer */
#define CMD_RESERVE 1024                /* room one draw's state can take */
#define MAX_DIM     1024

typedef struct PTex {
    uint32_t pa, pw, ph, bytes;
    int w, h;
    unsigned seq;
} PTex;

typedef struct PTarget {
    const uint32_t *key;                /* rt->color */
    int w, h, pw, ph;
    uint32_t color_pa, lin_pa;
    int gpu_valid;                      /* the GPU buffer holds the image */
    int dirty;                          /* ...and it is newer than rt->color */
    RT rt;                              /* where to write it back */
    unsigned used;
} PTarget;

typedef struct PDepth {
    const uint16_t *key;                /* rt->depth */
    int pw, ph;
    uint32_t pa;
    unsigned used;
} PDepth;

/* everything that is register state for one draw */
typedef struct DState {
    uint32_t vp[4];
    int sc_on;
    uint32_t sc[4];
    float clip[16];
    pica_raster_t rs;
    unsigned units;
    pica_tex_t tex[2];
    float tscale[2][2];
    pica_tev_t tev[PICA_TEV_STAGES];
} DState;

static struct {
    const pica_dev_t *dev;
    int state;                          /* 0 = not probed, 1 = up, -1 = unusable */
    uint32_t vbuf_pa;
    Vtx *vstage;
    int nvtx;
    uint32_t *list;
    pica_cmd_t cmd;
    int list_open;
    PTarget *cur;                       /* target of the open list */
    PDepth *cur_depth;
    DState last;
    int last_valid;
    PTarget target[MAX_TARGETS];
    PDepth depth[MAX_DEPTHS];
    uint32_t *scratch;
    size_t scratch_words;
    unsigned tick, warned;
    unsigned lists, draws;
} P;

enum { WARN_FOG = 1, WARN_TEXENV = 2, WARN_BIG = 4, WARN_MEM = 8, WARN_TEXBIG = 16 };

static void warn_once(unsigned bit, const char *msg)
{
    if (P.warned & bit)
        return;
    P.warned |= bit;
    gl_log("pica: %s", msg);
}

/* A GPU operation failed: say so and stop using the GPU. */
static void gpu_fail(const char *what, uint32_t st)
{
    if (P.state == 1)
        gl_log("pica: %s failed: %s. GPU rendering is OFF from here on, using the software rasterizer",
               what, pica_strerror(st));
    P.state = -1;
    P.list_open = 0;
    P.nvtx = 0;
}

static uint32_t *scratch(size_t words)
{
    if (words > P.scratch_words) {
        free(P.scratch);
        P.scratch = malloc(words * 4);
        P.scratch_words = P.scratch ? words : 0;
    }
    return P.scratch;
}

/* A constant-colour quad over a 64x64 RGBA8 target at `color`: the list a
   clear is made of (no PSC fills: the fill engine cannot write FCRAM, where
   the arena is - hardware run 13). Returns the list size in bytes. */
static uint32_t solid_list(uint32_t *list, uint32_t cap, uint32_t color, uint32_t vbuf, uint32_t tev_color)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    pica_cmd_t c;
    pica_fb_t fb;
    pica_raster_t rs;
    pica_tev_t tev[PICA_TEV_STAGES];
    pica_vtx_layout_t l;
    float clip[16];
    int i;
    pica_cmd_init(&c, list, cap);
    pica_emit_base_state(&c);
    pica_ff_emit_program(&c);
    memset(&fb, 0, sizeof fb);
    fb.color_pa = color;
    fb.width = fb.height = 64;
    fb.color_fmt = PICA_CB_RGBA8;
    pica_emit_framebuffer(&c, &fb);
    pica_emit_viewport(&c, 0, 0, 64, 64);
    pica_emit_scissor(&c, 0, 0, 0, 0, 0);
    pica_raster_defaults(&rs);
    rs.write_mask = PICA_WRITE_COLOR;
    pica_emit_raster(&c, &rs);
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&tev[i]);
    tev[0].src_rgb[0] = tev[0].src_a[0] = PICA_TEVSRC_CONSTANT;
    tev[0].color = tev_color;
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(&c, (unsigned)i, &tev[i]);
    pica_emit_texunits(&c, 0);
    pica_ff_clip_matrix(clip, ident, ident);
    pica_ff_emit_matrices(&c, clip, NULL, NULL, 1.0f);
    memset(&l, 0, sizeof l);
    l.count = PICA_FF_ATTRS;
    l.attr[PICA_FF_POSITION].type = PICA_FLOAT;  l.attr[PICA_FF_POSITION].size = 4;
    l.attr[PICA_FF_COLOR].type = PICA_FLOAT;     l.attr[PICA_FF_COLOR].size = 4;
    l.attr[PICA_FF_TEXCOORD0].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD0].size = 2;
    l.attr[PICA_FF_TEXCOORD1].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD1].size = 2;
    l.nbuffers = 1;
    l.buffer[0].stride = sizeof(Vtx);
    l.base_pa = vbuf;
    pica_emit_vtx_layout(&c, &l);
    pica_emit_draw_arrays(&c, PICA_TRIANGLES, 0, 6);
    pica_emit_fb_flush(&c);
    return pica_cmd_finish(&c);
}

/* Before the first draw: one round trip through everything every frame
   relies on and the model cannot vouch for on hardware - the transport, the
   shim's cache maintenance, the PPF in both directions (8x8 tiling) and a
   P3D draw into the arena (FCRAM). 64x64 RGBA8: a pattern up and back, then
   a constant-colour quad and back. 0 = usable; otherwise logs what came
   back and the caller uses the CPU. */
static int self_check(void)
{
    enum { N = 64 * 64 };
    uint32_t a = 0, b = 0, st, i, *buf = malloc(N * 4), dim = 64u << 16 | 64u, bad = 0, first = 0;
    const char *what = "allocating";
    if (!buf)
        return -1;
    st = P.dev->alloc(N * 4, 0, &a);
    if (st == PICA_OK)
        st = P.dev->alloc(N * 4, 0, &b);
    for (i = 0; i < N; i++)
        buf[i] = i * 2654435761u ^ 0x5A5AA5A5u;
    if (st == PICA_OK && (what = "upload", st = P.dev->write(a, buf, N * 4)) == PICA_OK &&
        (what = "linear->tiled transfer", st = P.dev->transfer(a, dim, b, dim, PPF_OUT_TILED |
            PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
        (what = "tiled->linear transfer", st = P.dev->transfer(b, dim, a, dim,
            PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
        (what = "read-back", st = P.dev->read(a, buf, N * 4)) == PICA_OK) {
        for (i = 0; i < N; i++)
            if (buf[i] != (i * 2654435761u ^ 0x5A5AA5A5u) && !bad++)
                first = i;
        if (bad) {
            gl_log("pica: GPU self-check: %u of %u words wrong after a transfer round trip (word %u: 0x%08x, "
                   "expected 0x%08x): not using the GPU", bad, (unsigned)N, first, buf[first],
                   first * 2654435761u ^ 0x5A5AA5A5u);
            st = PICA_ERR_RANGE;
            what = NULL;
        } else {
            static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
            uint32_t vb = 0, *list = malloc(4096 * 4), bytes = 0;
            Vtx q[6];
            memset(q, 0, sizeof q);
            for (i = 0; i < 6; i++) {
                q[i].x = corner[i][0];
                q[i].y = corner[i][1];
                q[i].w = 1;
            }
            what = "allocating";
            st = list ? P.dev->alloc(sizeof q, 0, &vb) : PICA_ERR_NO_MEM;
            if (st == PICA_OK && !(bytes = solid_list(list, 4096, b, vb, 0x87654321u)))
                st = PICA_ERR_RANGE;
            if (st == PICA_OK && (what = "a P3D draw", st = P.dev->submit(list, bytes, vb, q, sizeof q)) == PICA_OK &&
                (what = "tiled->linear transfer", st = P.dev->transfer(b, dim, a, dim,
                    PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8))) == PICA_OK &&
                (what = "read-back", st = P.dev->read(a, buf, N * 4)) == PICA_OK) {
                for (i = 0; i < N; i++)
                    if (buf[i] != 0x21436587u && !bad++)
                        first = i;
                if (bad) {
                    gl_log("pica: GPU self-check: a constant-colour quad left %u of %u words wrong (word %u: 0x%08x, "
                           "expected 0x21436587): not using the GPU", bad, (unsigned)N, first, buf[first]);
                    st = PICA_ERR_RANGE;
                    what = NULL;
                }
            }
            if (vb)
                P.dev->free(vb);
            free(list);
        }
    }
    if (st != PICA_OK && what)
        gl_log("pica: GPU self-check: %s failed: %s: not using the GPU", what, pica_strerror(st));
    if (a) P.dev->free(a);
    if (b) P.dev->free(b);
    free(buf);
    return st == PICA_OK ? 0 : -1;
}

int pica_backend_init(void)
{
    pica_info_t info;
    uint32_t st;
    if (P.state)
        return P.state > 0;
    P.state = -1;
#ifdef GL_HOST
    P.dev = pica_dev_host();
#else
    P.dev = pica_dev_ios();
#endif
    memset(&info, 0, sizeof info);
    st = P.dev->info(&info);
    if (st != PICA_OK || info.state != PICA_OK) {
        gl_log("pica: GPU not available: %s (PDN_GPU_CNT 0x%x)", pica_strerror(st != PICA_OK ? st : info.state),
               info.gpu_cnt);
        return 0;
    }
    if (info.version != PICA_PROTO_VERSION) {
        gl_log("pica: shim speaks protocol %u, this engine %u: not using the GPU", info.version, PICA_PROTO_VERSION);
        return 0;
    }
    if (self_check())
        return 0;
    P.vstage = malloc(sizeof(Vtx) * VTX_CAP);
    P.list = malloc(CMD_WORDS * 4);
    st = P.dev->alloc(sizeof(Vtx) * VTX_CAP, 0, &P.vbuf_pa);
    if (!P.vstage || !P.list || st != PICA_OK) {
        gl_log("pica: no memory for the vertex buffer (%s)", pica_strerror(st));
        return 0;
    }
    P.state = 1;
    gl_log("pica: GPU up (self-check passed): arena %u KiB at PA 0x%08x (%u KiB free), PDN_GPU_CNT 0x%x, %s",
           info.arena_size >> 10, info.arena_pa, info.arena_free >> 10, info.gpu_cnt, P.dev->name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* command list                                                        */
/* ------------------------------------------------------------------ */

static void flush_list(void)
{
    uint32_t bytes, st;
    if (!P.list_open)
        return;
    P.list_open = 0;
    pica_emit_fb_flush(&P.cmd);
    bytes = pica_cmd_finish(&P.cmd);
    if (!bytes) {
        gpu_fail("building a command list (overflow)", PICA_ERR_RANGE);
        return;
    }
    st = P.dev->submit(P.list, bytes, P.vbuf_pa, P.vstage, (uint32_t)(P.nvtx * sizeof(Vtx)));
    P.nvtx = 0;
    P.lists++;
    if (st != PICA_OK)
        gpu_fail("running a command list", st);
}

/* write the GPU image of a target back into its CPU memory */
static void sync_target(PTarget *t)
{
    uint32_t dim = (uint32_t)t->ph << 16 | (uint32_t)t->pw, st, *lin;
    int y, x;
    if (P.cur == t)
        flush_list();
    if (!t->dirty || P.state != 1)
        return;
    t->dirty = 0;
    lin = scratch((size_t)t->pw * t->ph);
    if (!lin)
        return;
    st = P.dev->transfer(t->color_pa, dim, t->lin_pa, dim, PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8));
    if (st == PICA_OK)
        st = P.dev->read(t->lin_pa, lin, (uint32_t)t->pw * t->ph * 4);
    if (st != PICA_OK) {
        gpu_fail("reading a frame back", st);
        return;
    }
    /* Transfer row r is GL row ph-1-r; a GPU word is R<<24|G<<16|B<<8|A and
       the engine's is A<<24|R<<16|G<<8|B: rotate right by 8. */
    for (y = 0; y < t->h; y++) {
        const uint32_t *s = lin + (size_t)(t->ph - 1 - y) * t->pw;
        uint32_t *d = t->rt.color + (size_t)(t->rt.flip ? t->h - 1 - y : y) * t->rt.pitch;
        for (x = 0; x < t->w; x++)
            d[x] = (s[x] >> 8 | s[x] << 24) | t->rt.opaque;
    }
}

/* bring rt->color into the GPU buffer */
static int load_target(PTarget *t)
{
    uint32_t dim = (uint32_t)t->ph << 16 | (uint32_t)t->pw, st, *lin = scratch((size_t)t->pw * t->ph);
    int y, x;
    if (!lin)
        return 0;
    memset(lin, 0, (size_t)t->pw * t->ph * 4);
    for (y = 0; y < t->h; y++) {
        uint32_t *d = lin + (size_t)(t->ph - 1 - y) * t->pw;
        const uint32_t *s = t->rt.color + (size_t)(t->rt.flip ? t->h - 1 - y : y) * t->rt.pitch;
        for (x = 0; x < t->w; x++)
            d[x] = s[x] << 8 | s[x] >> 24;
    }
    st = P.dev->write(t->lin_pa, lin, (uint32_t)t->pw * t->ph * 4);
    if (st == PICA_OK)
        st = P.dev->transfer(t->lin_pa, dim, t->color_pa, dim,
                             PPF_OUT_TILED | PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8));
    if (st != PICA_OK) {
        gpu_fail("uploading a render target", st);
        return 0;
    }
    t->gpu_valid = 1;
    return 1;
}

static void drop_target(PTarget *t)
{
    if (P.cur == t) {
        P.cur = NULL;
        P.list_open = 0;                /* its pending draws can no longer be seen */
        P.nvtx = 0;
    }
    if (t->color_pa) P.dev->free(t->color_pa);
    if (t->lin_pa) P.dev->free(t->lin_pa);
    memset(t, 0, sizeof *t);
}

static void drop_depth(PDepth *d)
{
    if (P.cur_depth == d) {
        flush_list();
        P.cur_depth = NULL;
    }
    if (d->pa) P.dev->free(d->pa);
    memset(d, 0, sizeof *d);
}

/* The engine is about to free or reuse this memory (renderbuffer storage,
   texture image): forget GPU copies keyed on it. */
void pica_forget(const void *mem)
{
    int i;
    if (!mem || P.state != 1)
        return;
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key == mem)
            drop_target(&P.target[i]);
    for (i = 0; i < MAX_DEPTHS; i++)
        if (P.depth[i].key == mem)
            drop_depth(&P.depth[i]);
}

static int pad_dim(int n)
{
    n = (n + 15) & ~15;
    return n < 64 ? 64 : n;             /* display transfers below 64x64 are unreliable */
}

static PTarget *get_target(const RT *rt)
{
    PTarget *t = NULL, *lru = &P.target[0];
    uint32_t bytes;
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        PTarget *c = &P.target[i];
        if (c->key == rt->color && c->w == rt->w && c->h == rt->h) {
            t = c;
            break;
        }
        if (c->used < lru->used)
            lru = c;
    }
    if (!t) {
        if (rt->w > MAX_DIM || rt->h > MAX_DIM) {
            warn_once(WARN_BIG, "render target larger than 1024: drawn by the software rasterizer");
            return NULL;
        }
        t = lru;
        if (t->key) {
            sync_target(t);
            drop_target(t);
        }
        t->pw = pad_dim(rt->w);
        t->ph = pad_dim(rt->h);
        bytes = (uint32_t)t->pw * t->ph * 4;
        if (P.dev->alloc(bytes, 0, &t->color_pa) != PICA_OK || P.dev->alloc(bytes, 0, &t->lin_pa) != PICA_OK) {
            warn_once(WARN_MEM, "GPU arena full (render target): drawn by the software rasterizer");
            drop_target(t);
            return NULL;
        }
        t->key = rt->color;
        t->w = rt->w;
        t->h = rt->h;
    }
    t->rt = *rt;
    t->used = ++P.tick;
    return t;
}

static PDepth *get_depth(const RT *rt, const PTarget *t)
{
    PDepth *d = NULL, *lru = &P.depth[0];
    uint32_t bytes = (uint32_t)t->pw * t->ph * 4;
    int i;
    if (!rt->depth)
        return NULL;
    for (i = 0; i < MAX_DEPTHS; i++) {
        PDepth *c = &P.depth[i];
        if (c->key == rt->depth && c->pw == t->pw && c->ph == t->ph) {
            d = c;
            break;
        }
        if (c->used < lru->used)
            lru = c;
    }
    if (!d) {
        d = lru;
        if (d->key)
            drop_depth(d);
        if (P.dev->alloc(bytes, 0, &d->pa) != PICA_OK) {
            warn_once(WARN_MEM, "GPU arena full (depth buffer): depth test disabled");
            memset(d, 0, sizeof *d);
            return NULL;
        }
        d->key = rt->depth;
        d->pw = t->pw;
        d->ph = t->ph;
        {   /* far: a constant, so tiling does not matter; written by the CPU because
               the fill engine cannot reach the (FCRAM) arena */
            uint32_t *z = scratch(bytes / 4), k;
            if (z) {
                for (k = 0; k < bytes / 4; k++)
                    z[k] = 0x00FFFFFF;
                P.dev->write(d->pa, z, bytes);
            }
        }
    }
    d->used = ++P.tick;
    return d;
}

/* Make rt the target of the open command list. NULL: use the software path. */
static PTarget *bind_target(const RT *rt)
{
    PTarget *t;
    PDepth *d;
    if (P.state != 1)
        return NULL;
    if (P.cur && (P.cur->key != rt->color || P.cur->w != rt->w || P.cur->h != rt->h))
        sync_target(P.cur);             /* one target is ahead of its CPU copy at a time */
    t = get_target(rt);
    if (!t || P.state != 1)
        return NULL;
    d = get_depth(rt, t);
    if (t != P.cur || d != P.cur_depth) {
        flush_list();
        P.cur = t;
        P.cur_depth = d;
    }
    return P.state == 1 ? t : NULL;
}

static void open_list(void)
{
    pica_fb_t fb;
    if (P.list_open)
        return;
    pica_cmd_init(&P.cmd, P.list, CMD_WORDS);
    pica_emit_base_state(&P.cmd);
    pica_ff_emit_program(&P.cmd);
    memset(&fb, 0, sizeof fb);
    fb.color_pa = P.cur->color_pa;
    fb.depth_pa = P.cur_depth ? P.cur_depth->pa : 0;
    fb.width = (uint16_t)P.cur->pw;
    fb.height = (uint16_t)P.cur->ph;
    fb.color_fmt = PICA_CB_RGBA8;
    fb.depth_fmt = PICA_DEPTH24S8;
    pica_emit_framebuffer(&P.cmd, &fb);
    P.list_open = 1;
    P.last_valid = 0;
    P.nvtx = 0;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

static void pica_tex_deleted(Tex *t)
{
    PTex *p = t->be_priv;
    if (!p)
        return;
    if (P.state == 1) {
        flush_list();                   /* pending draws may sample it */
        P.dev->free(p->pa);
    }
    free(p);
    t->be_priv = NULL;
}

static void pica_tex_changed(GC *gc, Tex *t)
{
    (void)gc;
    /* the image of a render-to-texture target was replaced on the CPU side */
    if (t->px && P.state == 1) {
        int i;
        for (i = 0; i < MAX_TARGETS; i++)
            if (P.target[i].key == t->px) {
                if (P.cur == &P.target[i])
                    flush_list();
                P.target[i].gpu_valid = P.target[i].dirty = 0;
            }
    }
}

/* the tiled GPU copy of a texture, uploaded when the image changed */
static PTex *use_texture(Tex *t)
{
    PTex *p = t->be_priv;
    uint32_t pw = pica_pot((uint32_t)t->w), ph = pica_pot((uint32_t)t->h), bytes = pw * ph * 4, st;
    void *tiled;
    if (pw > MAX_DIM || ph > MAX_DIM) {
        warn_once(WARN_TEXBIG, "texture larger than 1024: sampled as opaque black");
        return NULL;
    }
    if (p && p->seq == t->be_seq && p->w == t->w && p->h == t->h)
        return p;
    flush_list();                       /* earlier draws must see the old texels */
    if (P.state != 1)
        return NULL;
    if (p && p->bytes != bytes) {
        P.dev->free(p->pa);
        free(p);
        t->be_priv = p = NULL;
    }
    if (!p) {
        p = calloc(1, sizeof *p);
        if (!p || P.dev->alloc(bytes, 0, &p->pa) != PICA_OK) {
            warn_once(WARN_MEM, "GPU arena full (texture): sampled as opaque black");
            free(p);
            return NULL;
        }
        p->bytes = bytes;
        t->be_priv = p;
    }
    tiled = scratch(pw * ph);
    if (!tiled)
        return NULL;
    if ((uint32_t)t->w != pw || (uint32_t)t->h != ph)
        memset(tiled, 0, bytes);
    /* px words are 0xAARRGGBB = bytes B, G, R, A */
    pica_tex_upload(tiled, PICA_TEX_RGBA8, pw, ph, 0, 0, t->px, PICA_SRC_BGRA8888,
                    (uint32_t)t->w, (uint32_t)t->h, (uint32_t)t->pitch * 4);
    if ((uint32_t)t->w != pw || (uint32_t)t->h != ph)
        pica_tex_pad_edges(tiled, PICA_TEX_RGBA8, pw, ph, (uint32_t)t->w, (uint32_t)t->h);
    st = P.dev->write(p->pa, tiled, bytes);
    if (st != PICA_OK) {
        gpu_fail("uploading a texture", st);
        return NULL;
    }
    p->pw = pw;
    p->ph = ph;
    p->w = t->w;
    p->h = t->h;
    p->seq = t->be_seq;
    P.last_valid = 0;                   /* re-send the texture state: drops the GPU's texel cache */
    return p;
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static uint32_t rgba_u32(const float c[4])
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        v |= (uint32_t)(f * 255.0f + 0.5f) << (8 * i);
    }
    return v;
}

static int ifunc(GLenum f, int dflt)
{
    int v = pica_gl_func(f);
    return v < 0 ? dflt : v;
}

static int ifactor(GLenum f, int dflt)
{
    int v = pica_gl_blend_factor(f);
    return v < 0 ? dflt : v;
}

/* Viewport, scissor and the clip matrix. The GPU's viewport origin cannot be
   negative and must lie inside the target, so a GL viewport that does not
   fit is replaced by the whole target plus a scale/offset folded into the
   clip matrix, and the scissor box takes over the clipping. */
static void state_viewport(DState *s, const GC *gc, const PTarget *t, const int vp[4], int scissor, const int sc[4])
{
    float m[16], ident[16];
    int x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;
    int gl_scissor = scissor && sc;     /* the GL scissor test itself is on */
    int fits = vp[0] >= 0 && vp[1] >= 0 && vp[2] > 0 && vp[3] > 0 && vp[0] + vp[2] <= t->pw && vp[1] + vp[3] <= t->ph;
    memset(m, 0, sizeof m);
    memset(ident, 0, sizeof ident);
    m[0] = m[5] = m[10] = m[15] = 1;
    ident[0] = ident[5] = ident[10] = ident[15] = 1;
    (void)gc;
    if (fits) {
        s->vp[0] = (uint32_t)vp[0]; s->vp[1] = (uint32_t)vp[1]; s->vp[2] = (uint32_t)vp[2]; s->vp[3] = (uint32_t)vp[3];
    } else {
        s->vp[0] = s->vp[1] = 0;
        s->vp[2] = (uint32_t)t->pw;
        s->vp[3] = (uint32_t)t->ph;
        /* window x = vx + (x/w + 1) vw/2  must equal  (x'/w + 1) pw/2 */
        m[0] = (float)vp[2] / (float)t->pw;
        m[12] = (2.0f * (float)vp[0] + (float)vp[2]) / (float)t->pw - 1.0f;
        m[5] = (float)vp[3] / (float)t->ph;
        m[13] = (2.0f * (float)vp[1] + (float)vp[3]) / (float)t->ph - 1.0f;
        if (vp[0] > x0) x0 = vp[0];
        if (vp[1] > y0) y0 = vp[1];
        if (vp[0] + vp[2] < x1) x1 = vp[0] + vp[2];
        if (vp[1] + vp[3] < y1) y1 = vp[1] + vp[3];
        scissor = 1;
    }
    if (gl_scissor) {
        if (sc[0] > x0) x0 = sc[0];
        if (sc[1] > y0) y0 = sc[1];
        if (sc[0] + sc[2] < x1) x1 = sc[0] + sc[2];
        if (sc[1] + sc[3] < y1) y1 = sc[1] + sc[3];
    }
    s->sc_on = scissor;
    if (scissor && x1 > x0 && y1 > y0) {
        s->sc[0] = (uint32_t)x0; s->sc[1] = (uint32_t)y0; s->sc[2] = (uint32_t)(x1 - x0); s->sc[3] = (uint32_t)(y1 - y0);
    }                                   /* else w = h = 0: nothing is drawn */
    pica_ff_clip_matrix(s->clip, m, ident);
}

static void build_state(DState *s, GC *gc, const RT *rt, const PTarget *t)
{
    const pica_gl_texenv_t *units[2] = { NULL, NULL };
    pica_gl_texenv_t env[2];
    pica_raster_t *r = &s->rs;
    int i, v;

    memset(s, 0, sizeof *s);
    state_viewport(s, gc, t, gc->vp, gc->scissor_on, gc->scissor);

    pica_raster_defaults(r);
    if (gc->cull && gc->cull_mode != GL_FRONT_AND_BACK)
        r->cull = (gc->cull_mode == GL_BACK) == (gc->front_face == GL_CCW) ? PICA_CULL_BACK_CCW : PICA_CULL_FRONT_CCW;
    r->depth_test = gc->depth_test && P.cur_depth;
    r->depth_func = (uint8_t)ifunc(gc->depth_func, PICA_LESS);
    r->write_mask = (uint8_t)((gc->color_mask[0] ? PICA_WRITE_R : 0) | (gc->color_mask[1] ? PICA_WRITE_G : 0) |
                              (gc->color_mask[2] ? PICA_WRITE_B : 0) |
                              (gc->color_mask[3] && !rt->opaque ? PICA_WRITE_A : 0) |
                              (gc->depth_mask ? PICA_WRITE_DEPTH : 0));
    r->alpha_test = (uint8_t)gc->alpha_test;
    r->alpha_func = (uint8_t)ifunc(gc->alpha_func, PICA_ALWAYS);
    v = (int)(gc->alpha_ref * 255.0f + 0.5f);
    r->alpha_ref = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    r->blend = (uint8_t)gc->blend;
    if (gc->blend) {
        v = pica_gl_blend_eq(gc->blend_eq_rgb); r->blend_eq_rgb = (uint8_t)(v < 0 ? PICA_BLEND_ADD : v);
        v = pica_gl_blend_eq(gc->blend_eq_a);   r->blend_eq_a = (uint8_t)(v < 0 ? PICA_BLEND_ADD : v);
        r->blend_src_rgb = (uint8_t)ifactor(gc->blend_src_rgb, PICA_BF_ONE);
        r->blend_dst_rgb = (uint8_t)ifactor(gc->blend_dst_rgb, PICA_BF_ZERO);
        r->blend_src_a = (uint8_t)ifactor(gc->blend_src_a, PICA_BF_ONE);
        r->blend_dst_a = (uint8_t)ifactor(gc->blend_dst_a, PICA_BF_ZERO);
    }
    r->depth_near = gc->depth_near;
    r->depth_far = gc->depth_far;
    /* glPolygonOffset: only the constant term exists on this GPU; the slope
       term is approximated by one unit per unit of factor */
    if (gc->poly_offset_on)
        r->depth_bias = (gc->poly_offset_units + gc->poly_offset_factor) / 16777215.0f;

    for (i = 0; i < 2; i++) {
        const TexUnit *tu = &gc->tu[i];
        Tex *tx = tu->enabled ? gl_bound_tex(gc, i) : NULL;
        PTex *p;
        pica_gl_texenv_t *e = &env[i];
        if (!tx || !tx->px || !tx->base)
            continue;
        p = use_texture(tx);
        if (P.state != 1)
            return;
        s->units |= 1u << i;
        if (p) {
            s->tex[i].pa = p->pa;
            s->tex[i].width = (uint16_t)p->pw;
            s->tex[i].height = (uint16_t)p->ph;
            s->tscale[i][0] = (float)p->w / (float)p->pw;
            s->tscale[i][1] = (float)p->h / (float)p->ph;
        } else {                        /* no GPU copy: address 0 samples opaque black */
            s->tex[i].width = s->tex[i].height = 8;
            s->tscale[i][0] = s->tscale[i][1] = 1;
        }
        s->tex[i].fmt = PICA_TEX_RGBA8;
        s->tex[i].mag = tx->mag_filter == GL_LINEAR ? PICA_LINEAR : PICA_NEAREST;
        s->tex[i].min = tx->min_filter == GL_NEAREST || tx->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                        tx->min_filter == GL_NEAREST_MIPMAP_LINEAR ? PICA_NEAREST : PICA_LINEAR;
        v = pica_gl_wrap(tx->wrap_s); s->tex[i].wrap_s = (uint8_t)(v < 0 ? PICA_REPEAT : v);
        v = pica_gl_wrap(tx->wrap_t); s->tex[i].wrap_t = (uint8_t)(v < 0 ? PICA_REPEAT : v);
        memset(e, 0, sizeof *e);
        e->mode = tu->env.mode;
        e->base_format = tx->base;
        e->env_color = rgba_u32(tu->env.color);
        e->comb_rgb = tu->env.comb_rgb;
        e->comb_a = tu->env.comb_a;
        for (v = 0; v < 3; v++) {
            e->src_rgb[v] = tu->env.src_rgb[v];
            e->src_a[v] = tu->env.src_a[v];
            e->op_rgb[v] = tu->env.op_rgb[v];
            e->op_a[v] = tu->env.op_a[v];
        }
        e->scale_rgb = (uint8_t)tu->env.rgb_scale;
        e->scale_a = (uint8_t)tu->env.a_scale;
        units[i] = e;
    }
    if (pica_ff_texenv(s->tev, units) < 0) {
        warn_once(WARN_TEXENV, "unsupported glTexEnv combination: drawn as GL_MODULATE");
        for (i = 0; i < 2; i++)
            if (units[i])
                env[i].mode = GL_MODULATE;
        pica_ff_texenv(s->tev, units);
    }
    if (gc->fog)
        warn_once(WARN_FOG, "glFog is not implemented on the GPU path yet: drawn without fog");
}

static void emit_state(const DState *s)
{
    pica_cmd_t *c = &P.cmd;
    float tm[2][16];
    int i;
    if (P.last_valid && !memcmp(s, &P.last, sizeof *s))
        return;
    pica_emit_viewport(c, s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    pica_emit_scissor(c, s->sc_on, s->sc[0], s->sc[1], s->sc[2], s->sc[3]);
    pica_emit_raster(c, &s->rs);
    for (i = 0; i < 2; i++) {
        memset(tm[i], 0, sizeof tm[i]);
        tm[i][0] = tm[i][5] = tm[i][10] = tm[i][15] = 1;
        if (s->units & (1u << i)) {
            pica_emit_texture(c, (unsigned)i, &s->tex[i]);
            tm[i][0] = s->tscale[i][0];
            tm[i][5] = s->tscale[i][1];
        }
    }
    pica_emit_texunits(c, s->units);
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(c, (unsigned)i, &s->tev[i]);
    pica_ff_emit_matrices(c, s->clip, tm[0], tm[1], 1.0f);
    P.last = *s;
    P.last_valid = 1;
}

/* the vertex format is struct Vtx itself */
static void emit_draw(const DState *s, const Vtx *v, int n)
{
    while (n > 0 && P.state == 1) {
        pica_vtx_layout_t l;
        int k;
        if (P.list_open && (P.nvtx + 3 > VTX_CAP || P.cmd.len + CMD_RESERVE > CMD_WORDS))
            flush_list();
        if (P.state != 1)
            return;
        if (!P.list_open) {
            open_list();
            memset(&l, 0, sizeof l);
            l.count = PICA_FF_ATTRS;
            l.attr[PICA_FF_POSITION].type = PICA_FLOAT;  l.attr[PICA_FF_POSITION].size = 4;
            l.attr[PICA_FF_COLOR].type = PICA_FLOAT;     l.attr[PICA_FF_COLOR].size = 4;
            l.attr[PICA_FF_TEXCOORD0].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD0].size = 2;
            l.attr[PICA_FF_TEXCOORD1].type = PICA_FLOAT; l.attr[PICA_FF_TEXCOORD1].size = 2;
            l.nbuffers = 1;
            l.buffer[0].stride = sizeof(Vtx);
            l.base_pa = P.vbuf_pa;
            pica_emit_vtx_layout(&P.cmd, &l);
        }
        emit_state(s);
        k = VTX_CAP - P.nvtx;
        k -= k % 3;
        if (k > n)
            k = n;
        memcpy(P.vstage + P.nvtx, v, (size_t)k * sizeof(Vtx));
        pica_emit_draw_arrays(&P.cmd, PICA_TRIANGLES, (uint32_t)P.nvtx, (uint32_t)k);
        P.nvtx += k;
        P.draws++;
        v += k;
        n -= k;
    }
}

/* ------------------------------------------------------------------ */
/* Backend                                                             */
/* ------------------------------------------------------------------ */

static void pica_triangles(GC *gc, RT *rt, const Vtx *v, int n)
{
    PTarget *t = bind_target(rt);
    DState s;
    if (t && !t->gpu_valid)
        load_target(t);
    if (!t || P.state != 1) {
        sw_backend.triangles(gc, rt, v, n);
        return;
    }
    if (gc->cull && gc->cull_mode == GL_FRONT_AND_BACK)
        return;
    build_state(&s, gc, rt, t);
    if (P.state != 1) {
        sw_backend.triangles(gc, rt, v, n);
        return;
    }
    emit_draw(&s, v, n);
    t->dirty = 1;
}

static void pica_clear(GC *gc, RT *rt, GLbitfield mask)
{
    PTarget *t = bind_target(rt);
    int color = (mask & GL_COLOR_BUFFER_BIT) != 0, depth, full, all_ch, i;
    float cc[4];
    if (!t) {
        sw_backend.clear(gc, rt, mask);
        return;
    }
    depth = (mask & GL_DEPTH_BUFFER_BIT) && P.cur_depth && gc->depth_mask;
    if (!color && !depth)
        return;
    for (i = 0; i < 4; i++)
        cc[i] = gc->clear_color[i];
    if (rt->opaque)
        cc[3] = 1;
    full = !gc->scissor_on || (gc->scissor[0] <= 0 && gc->scissor[1] <= 0 &&
                               gc->scissor[0] + gc->scissor[2] >= t->w && gc->scissor[1] + gc->scissor[3] >= t->h);
    all_ch = gc->color_mask[0] && gc->color_mask[1] && gc->color_mask[2] && (gc->color_mask[3] || rt->opaque);

    /* Every clear is a quad at the clear depth (depth function ALWAYS). The
       PSC fill engine would be faster but cannot write FCRAM, and the arena
       is FCRAM (hardware run 13). A clear of all colour channels over the
       whole target needs no upload of the old picture first. */
    if (full && color && all_ch)
        t->gpu_valid = 1;

    /* a quad at the clear depth */
    {
        static const float corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        static const int whole_vp[4] = { 0, 0, 0, 0 };
        DState s;
        Vtx q[6];
        int vp[4];
        float z = (gc->clear_depth < 0 ? 0 : gc->clear_depth > 1 ? 1 : gc->clear_depth) * 2.0f - 1.0f;
        (void)whole_vp;
        if (!t->gpu_valid && !load_target(t)) {
            sw_backend.clear(gc, rt, mask);
            return;
        }
        memset(&s, 0, sizeof s);
        vp[0] = vp[1] = 0; vp[2] = t->pw; vp[3] = t->ph;
        state_viewport(&s, gc, t, vp, 1, gc->scissor_on ? gc->scissor : vp);
        pica_raster_defaults(&s.rs);
        s.rs.depth_test = (uint8_t)depth;
        s.rs.depth_func = PICA_ALWAYS;
        s.rs.write_mask = (uint8_t)((depth ? PICA_WRITE_DEPTH : 0) |
            (!color ? 0 : (gc->color_mask[0] ? PICA_WRITE_R : 0) | (gc->color_mask[1] ? PICA_WRITE_G : 0) |
                          (gc->color_mask[2] ? PICA_WRITE_B : 0) | (gc->color_mask[3] && !rt->opaque ? PICA_WRITE_A : 0)));
        pica_ff_texenv(s.tev, (const pica_gl_texenv_t *[2]){ NULL, NULL });
        memset(q, 0, sizeof q);
        for (i = 0; i < 6; i++) {
            q[i].x = corner[i][0]; q[i].y = corner[i][1]; q[i].z = z; q[i].w = 1;
            memcpy(q[i].c, cc, sizeof cc);
            q[i].fog = 1;
        }
        emit_draw(&s, q, 6);
        if (P.state != 1) {
            sw_backend.clear(gc, rt, mask);
            return;
        }
        t->dirty = 1;
    }
}

static void pica_finish(GC *gc, RT *rt)
{
    int i;
    (void)gc;
    if (P.state != 1)
        return;
    for (i = 0; i < MAX_TARGETS; i++)
        if (P.target[i].key == rt->color && P.target[i].w == rt->w && P.target[i].h == rt->h) {
            P.target[i].rt = *rt;
            sync_target(&P.target[i]);
        }
}

const Backend pica_backend = {
    "ios3ds PICA200",
    pica_clear,
    pica_triangles,
    pica_tex_changed,
    pica_tex_deleted,
    pica_finish,
};

void pica_backend_stats(unsigned *lists, unsigned *draws)
{
    *lists = P.lists;
    *draws = P.draws;
}
