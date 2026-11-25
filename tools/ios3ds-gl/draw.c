/* ios3ds GL engine front end: vertex fetch, transform, lighting, fog and
 * primitive assembly. Output: clip-space triangles for the Backend.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define BATCH 126               /* vertices per Backend call (multiple of 3) */

typedef struct Fetch {
    const uint8_t *p;
    int stride, size;
    GLenum type;
} Fetch;

static int type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    }
    return 4;
}

static int fetch_init(GC *gc, Fetch *f, const Array *a)
{
    const uint8_t *base = a->ptr;
    if (!a->enabled)
        return 0;
    if (a->buf) {
        Buf *b = nt_get(&gc->share->buf, a->buf);
        if (!b || !b->data)
            return 0;
        base = b->data + (size_t)a->ptr;
    }
    if (!base)
        return 0;
    f->p = base;
    f->size = a->size;
    f->type = a->type;
    f->stride = a->stride ? a->stride : a->size * type_size(a->type);
    return 1;
}

/* Read f->size components of element i as floats; norm: map integers to 0..1 / -1..1. */
static void fetch(const Fetch *f, int i, float *out, int norm)
{
    const uint8_t *p = f->p + (size_t)i * f->stride;
    int k;
    for (k = 0; k < f->size; k++) {
        switch (f->type) {
        case GL_FLOAT:
            memcpy(&out[k], p + 4 * k, 4);
            break;
        case GL_FIXED: {
            int32_t v;
            memcpy(&v, p + 4 * k, 4);
            out[k] = v * (1.0f / 65536.0f);
            break;
        }
        case GL_SHORT: {
            int16_t v;
            memcpy(&v, p + 2 * k, 2);
            out[k] = norm ? v * (1.0f / 32767.0f) : v;
            break;
        }
        case GL_UNSIGNED_SHORT: {
            uint16_t v;
            memcpy(&v, p + 2 * k, 2);
            out[k] = norm ? v * (1.0f / 65535.0f) : v;
            break;
        }
        case GL_BYTE:
            out[k] = norm ? (int8_t)p[k] * (1.0f / 127.0f) : (int8_t)p[k];
            break;
        default:
            out[k] = norm ? p[k] * (1.0f / 255.0f) : p[k];
            break;
        }
    }
}

static void mat_mul4(Mat4 *r, const Mat4 *a, const Mat4 *b)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r->m[i * 4 + j] = a->m[j] * b->m[i * 4] + a->m[4 + j] * b->m[i * 4 + 1] +
                              a->m[8 + j] * b->m[i * 4 + 2] + a->m[12 + j] * b->m[i * 4 + 3];
}

static inline float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *v)
{
    float l = dot3(v, v);
    if (l > 0) {
        l = 1 / sqrtf(l);
        v[0] *= l; v[1] *= l; v[2] *= l;
    }
}

/* Inverse transpose of the upper 3x3 of m (for normals), row major out[9]. */
static void normal_matrix(const Mat4 *m, float *o)
{
    float a = m->m[0], b = m->m[4], c = m->m[8], d = m->m[1], e = m->m[5], f = m->m[9], g = m->m[2],
          h = m->m[6], i = m->m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g), s;
    s = det != 0 ? 1 / det : 0;
    o[0] = (e * i - f * h) * s; o[1] = (f * g - d * i) * s; o[2] = (d * h - e * g) * s;
    o[3] = (c * h - b * i) * s; o[4] = (a * i - c * g) * s; o[5] = (b * g - a * h) * s;
    o[6] = (b * f - c * e) * s; o[7] = (c * d - a * f) * s; o[8] = (a * e - b * d) * s;
}

static void light_vertex(GC *gc, const float *eye, const float *n, const float *vcol, float *out)
{
    const Material *m = &gc->mat;
    const float *amb = gc->color_material ? vcol : m->ambient;
    const float *dif = gc->color_material ? vcol : m->diffuse;
    float V[3] = {0, 0, 1};
    int i, k;
    for (k = 0; k < 3; k++)
        out[k] = m->emission[k] + amb[k] * gc->light_model_ambient[k];
    out[3] = dif[3];
    for (i = 0; i < MAX_LIGHTS; i++) {
        const Light *l = &gc->light[i];
        float L[3], att = 1, nl, spec = 0;
        if (!l->enabled)
            continue;
        if (l->pos[3] == 0) {
            L[0] = l->pos[0]; L[1] = l->pos[1]; L[2] = l->pos[2];
            norm3(L);
        } else {
            float d;
            L[0] = l->pos[0] / l->pos[3] - eye[0];
            L[1] = l->pos[1] / l->pos[3] - eye[1];
            L[2] = l->pos[2] / l->pos[3] - eye[2];
            d = sqrtf(dot3(L, L));
            if (d > 0) {
                L[0] /= d; L[1] /= d; L[2] /= d;
            }
            att = 1 / (l->att[0] + l->att[1] * d + l->att[2] * d * d);
            if (l->spot_cutoff != 180) {
                float sd[3] = {l->spot_dir[0], l->spot_dir[1], l->spot_dir[2]}, c;
                norm3(sd);
                c = -dot3(L, sd);
                if (c < cosf(l->spot_cutoff * (float)(M_PI / 180)))
                    att = 0;
                else if (l->spot_exp != 0)
                    att *= powf(c, l->spot_exp);
            }
        }
        nl = dot3(n, L);
        if (nl < 0)
            nl = 0;
        if (nl > 0 && m->shininess >= 0 && (m->specular[0] || m->specular[1] || m->specular[2])) {
            float H[3] = {L[0] + V[0], L[1] + V[1], L[2] + V[2]}, nh;
            norm3(H);
            nh = dot3(n, H);
            if (nh > 0)
                spec = m->shininess == 0 ? 1 : powf(nh, m->shininess);
        }
        for (k = 0; k < 3; k++)
            out[k] += att * (amb[k] * l->ambient[k] + nl * dif[k] * l->diffuse[k] + spec * m->specular[k] * l->specular[k]);
    }
    for (k = 0; k < 4; k++)
        out[k] = sat(out[k]);
}

static const Mat4 *mvp_get(GC *gc)
{
    if (gc->mvp_dirty) {
        mat_mul4(&gc->mvp, &gc->proj[gc->proj_depth], &gc->mv[gc->mv_depth]);
        gc->mvp_dirty = 0;
    }
    return &gc->mvp;
}

/* Transform vertices [lo, hi] of the enabled arrays into gc->vcache[0 .. hi-lo]. */
static int transform(GC *gc, int lo, int hi)
{
    Fetch fv, fc, fn, ft[MAX_TEX_UNITS], fi, fw;
    int has_c, has_n, has_t[MAX_TEX_UNITS], use_t[MAX_TEX_UNITS], has_pal, i, u, n = hi - lo + 1;
    float rect_w[MAX_TEX_UNITS], rect_h[MAX_TEX_UNITS];
    const Mat4 *mvp = mvp_get(gc), *mv = &gc->mv[gc->mv_depth];
    float nm[9];
    int need_eye = gc->lighting || gc->fog;
    Vtx *out;

    if (!fetch_init(gc, &fv, &gc->a_vertex))
        return 0;
    if (n > gc->vcache_n) {
        Vtx *p = realloc(gc->vcache, (size_t)n * sizeof *p);
        if (!p) {
            gl_set_error(gc, GL_OUT_OF_MEMORY);
            return 0;
        }
        gc->vcache = p;
        gc->vcache_n = n;
    }
    has_c = fetch_init(gc, &fc, &gc->a_color);
    has_n = gc->lighting && fetch_init(gc, &fn, &gc->a_normal);
    has_pal = gc->a_matrix_idx.enabled && gc->a_weight.enabled &&
              fetch_init(gc, &fi, &gc->a_matrix_idx) && fetch_init(gc, &fw, &gc->a_weight);
    for (u = 0; u < MAX_TEX_UNITS; u++) {
        Tex *tx = gl_bound_tex(gc, u);
        use_t[u] = gc->tu[u].enabled;
        has_t[u] = use_t[u] && fetch_init(gc, &ft[u], &gc->a_texcoord[u]);
        rect_w[u] = use_t[u] && tx && tx->rect && tx->w ? (float)tx->w : 0;
        rect_h[u] = use_t[u] && tx && tx->rect && tx->h ? (float)tx->h : 1;
    }
    if (gc->lighting)
        normal_matrix(mv, nm);
    out = gc->vcache;
    for (i = 0; i < n; i++, out++) {
        float p[4] = {0, 0, 0, 1}, c[4], eye[3] = {0, 0, 0}, pn[3] = {0, 0, 0};
        int has_pn = 0;
        fetch(&fv, lo + i, p, 0);
        if (has_pal) {
            /* OES_matrix_palette: blend up to four object-space matrices
               with the per-vertex indices and weights, then use the blended
               position / normal as if it came from the vertex arrays. */
            float idx[4] = {0, 0, 0, 0}, wt[4] = {0, 0, 0, 0}, bp[4] = {0, 0, 0, 0}, bn[3] = {0, 0, 0};
            int k, j;
            fetch(&fi, lo + i, idx, 0);
            fetch(&fw, lo + i, wt, 0);
            for (k = 0; k < gc->a_matrix_idx.size && k < 4; k++) {
                int mi = (int)idx[k];
                const Mat4 *m;
                if (mi < 0 || mi >= MAX_PALETTE_MATRICES)
                    continue;
                m = &gc->palette[mi];
                for (j = 0; j < 4; j++)
                    bp[j] += wt[k] * (m->m[j] * p[0] + m->m[4 + j] * p[1] +
                                      m->m[8 + j] * p[2] + m->m[12 + j] * p[3]);
                for (j = 0; j < 3; j++)
                    bn[j] += wt[k] * (m->m[j] * p[0] + m->m[4 + j] * p[1] +
                                      m->m[8 + j] * p[2] + m->m[12 + j] * p[3]);
            }
            memcpy(p, bp, sizeof p);
            memcpy(pn, bn, sizeof pn);
            has_pn = 1;
        }
        out->x = mvp->m[0] * p[0] + mvp->m[4] * p[1] + mvp->m[8] * p[2] + mvp->m[12] * p[3];
        out->y = mvp->m[1] * p[0] + mvp->m[5] * p[1] + mvp->m[9] * p[2] + mvp->m[13] * p[3];
        out->z = mvp->m[2] * p[0] + mvp->m[6] * p[1] + mvp->m[10] * p[2] + mvp->m[14] * p[3];
        out->w = mvp->m[3] * p[0] + mvp->m[7] * p[1] + mvp->m[11] * p[2] + mvp->m[15] * p[3];
        if (need_eye) {
            eye[0] = mv->m[0] * p[0] + mv->m[4] * p[1] + mv->m[8] * p[2] + mv->m[12] * p[3];
            eye[1] = mv->m[1] * p[0] + mv->m[5] * p[1] + mv->m[9] * p[2] + mv->m[13] * p[3];
            eye[2] = mv->m[2] * p[0] + mv->m[6] * p[1] + mv->m[10] * p[2] + mv->m[14] * p[3];
        }
        if (has_c) {
            c[3] = 1;
            fetch(&fc, lo + i, c, 1);
        } else
            memcpy(c, gc->color, sizeof c);
        if (gc->lighting) {
            float nv[3], ne[3];
            if (has_pn)
                memcpy(nv, pn, sizeof nv);
            else if (has_n)
                fetch(&fn, lo + i, nv, 1);
            else
                memcpy(nv, gc->normal, sizeof nv);
            ne[0] = nm[0] * nv[0] + nm[1] * nv[1] + nm[2] * nv[2];
            ne[1] = nm[3] * nv[0] + nm[4] * nv[1] + nm[5] * nv[2];
            ne[2] = nm[6] * nv[0] + nm[7] * nv[1] + nm[8] * nv[2];
            if (gc->normalize || gc->rescale_normal)
                norm3(ne);
            light_vertex(gc, eye, ne, c, out->c);
        } else {
            out->c[0] = sat(c[0]); out->c[1] = sat(c[1]); out->c[2] = sat(c[2]); out->c[3] = sat(c[3]);
        }
        for (u = 0; u < MAX_TEX_UNITS; u++) {
            float t[4] = {0, 0, 0, 1};
            if (!use_t[u]) {
                out->t[u][0] = out->t[u][1] = 0;
                continue;
            }
            if (has_t[u])
                fetch(&ft[u], lo + i, t, 0);
            else
                memcpy(t, gc->texcoord[u], sizeof t);
            if (!gc->tu[u].identity) {
                const Mat4 *m = &gc->tu[u].stack[gc->tu[u].depth];
                float s = m->m[0] * t[0] + m->m[4] * t[1] + m->m[8] * t[2] + m->m[12] * t[3];
                float tt = m->m[1] * t[0] + m->m[5] * t[1] + m->m[9] * t[2] + m->m[13] * t[3];
                float q = m->m[3] * t[0] + m->m[7] * t[1] + m->m[11] * t[2] + m->m[15] * t[3];
                t[0] = s; t[1] = tt; t[3] = q;
            }
            if (t[3] != 1 && t[3] != 0) {
                t[0] /= t[3];
                t[1] /= t[3];
            }
            if (rect_w[u]) {                    /* rectangle texture: texel units */
                t[0] /= rect_w[u];
                t[1] /= rect_h[u];
            }
            out->t[u][0] = t[0];
            out->t[u][1] = t[1];
        }
        if (gc->fog) {
            float d = fabsf(eye[2]), f;
            if (gc->fog_mode == GL_LINEAR)
                f = gc->fog_end != gc->fog_start ? (gc->fog_end - d) / (gc->fog_end - gc->fog_start) : 0;
            else if (gc->fog_mode == GL_EXP2)
                f = expf(-(gc->fog_density * d) * (gc->fog_density * d));
            else
                f = expf(-gc->fog_density * d);
            out->fog = sat(f);
        } else
            out->fog = 1;
    }
    return 1;
}

typedef struct Emit {
    GC *gc;
    RT *rt;
    int cls;                        /* stereo stamp class for this draw (stereo.c) */
    Vtx buf[BATCH];
    int n;
} Emit;

static void emit_flush(Emit *e)
{
    if (e->n)
        gl_be_triangles(e->gc, e->rt, e->buf, e->n, e->cls);
    e->n = 0;
}

static void emit_tri(Emit *e, const Vtx *a, const Vtx *b, const Vtx *c)
{
    Vtx *o = &e->buf[e->n];
    o[0] = *a; o[1] = *b; o[2] = *c;
    if (e->gc->shade_model == GL_FLAT) {
        memcpy(o[0].c, c->c, sizeof o[0].c);
        memcpy(o[1].c, c->c, sizeof o[1].c);
    }
    e->n += 3;
    if (e->n == BATCH)
        emit_flush(e);
}

/* A screen-aligned quad of half size (hx, hy) pixels around clip-space point p. */
static void emit_point(Emit *e, const Vtx *p, float size)
{
    GC *gc = e->gc;
    Vtx q[4];
    float dx, dy;
    int i, u;
    if (p->w <= 0 || !gc->vp[2] || !gc->vp[3])
        return;
    dx = size / gc->vp[2] * p->w;
    dy = size / gc->vp[3] * p->w;
    for (i = 0; i < 4; i++) {
        q[i] = *p;
        q[i].x += (i == 1 || i == 2) ? dx : -dx;
        q[i].y += (i >= 2) ? dy : -dy;
        if (gc->point_sprite)
            for (u = 0; u < MAX_TEX_UNITS; u++) {
                q[i].t[u][0] = (i == 1 || i == 2) ? 1 : 0;
                q[i].t[u][1] = (i >= 2) ? 0 : 1;
            }
    }
    emit_tri(e, &q[0], &q[1], &q[2]);
    emit_tri(e, &q[0], &q[2], &q[3]);
}

static void lerp_vtx(Vtx *o, const Vtx *a, const Vtx *b, float t)
{
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *fo = (float *)o;
    unsigned i;
    for (i = 0; i < sizeof(Vtx) / sizeof(float); i++)
        fo[i] = fa[i] + (fb[i] - fa[i]) * t;
}

static void emit_line(Emit *e, const Vtx *a0, const Vtx *b0)
{
    GC *gc = e->gc;
    Vtx a = *a0, b = *b0, q[4];
    const float eps = 1e-5f;
    float ax, ay, bx, by, dx, dy, len, nx, ny;
    if (a.w < eps && b.w < eps)
        return;
    if (a.w < eps)
        lerp_vtx(&a, &a, &b, (eps - a.w) / (b.w - a.w));
    else if (b.w < eps)
        lerp_vtx(&b, &b, &a, (eps - b.w) / (a.w - b.w));
    ax = a.x / a.w; ay = a.y / a.w; bx = b.x / b.w; by = b.y / b.w;
    dx = (bx - ax) * gc->vp[2];
    dy = (by - ay) * gc->vp[3];
    len = sqrtf(dx * dx + dy * dy);
    if (len == 0 || !gc->vp[2] || !gc->vp[3])
        return;
    nx = -dy / len * gc->line_width / gc->vp[2];
    ny = dx / len * gc->line_width / gc->vp[3];
    q[0] = a; q[0].x -= nx * a.w; q[0].y -= ny * a.w;
    q[1] = a; q[1].x += nx * a.w; q[1].y += ny * a.w;
    q[2] = b; q[2].x += nx * b.w; q[2].y += ny * b.w;
    q[3] = b; q[3].x -= nx * b.w; q[3].y -= ny * b.w;
    if (gc->shade_model == GL_FLAT) {
        memcpy(q[0].c, b0->c, sizeof q[0].c);
        memcpy(q[1].c, b0->c, sizeof q[1].c);
    }
    emit_tri(e, &q[0], &q[1], &q[2]);
    emit_tri(e, &q[0], &q[2], &q[3]);
}

static inline unsigned idx_at(GLenum type, const void *ind, int first, int i)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:  return ((const uint8_t *)ind)[i];
    case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, (const uint8_t *)ind + 2 * i, 2); return v; }
    case GL_UNSIGNED_INT:   { uint32_t v; memcpy(&v, (const uint8_t *)ind + 4 * i, 4); return v; }
    }
    return first + i;
}

void gl_draw(GC *gc, GLenum mode, int first, int count, GLenum type, const void *ind)
{
    Emit *e;
    RT rt;
    unsigned lo, hi;
    int i, saved_cull = gc->cull;
    const Vtx *v;
#define V(i) (&v[idx_at(type, ind, first, (i)) - lo])

    if (count <= 0 || !gc->a_vertex.enabled)
        return;
    if (!gl_get_rt(gc, &rt)) {
        gl_set_error(gc, GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }
    if (type) {
        lo = ~0u; hi = 0;
        for (i = 0; i < count; i++) {
            unsigned k = idx_at(type, ind, 0, i);
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
        if (hi - lo > (1u << 20))
            return;
    } else {
        lo = first;
        hi = first + count - 1;
    }
    if (!transform(gc, lo, hi))
        return;
    if (!gc->emit && !(gc->emit = malloc(sizeof(Emit)))) {
        gl_set_error(gc, GL_OUT_OF_MEMORY);
        return;
    }
    e = gc->emit;
    v = gc->vcache;
    e->gc = gc;
    e->rt = &rt;
    e->cls = stereo_prepare(gc, &rt, v, hi - lo + 1);
    e->n = 0;
    switch (mode) {
    case GL_TRIANGLES:
        for (i = 0; i + 2 < count; i += 3)
            emit_tri(e, V(i), V(i + 1), V(i + 2));
        break;
    case GL_TRIANGLE_STRIP:
        for (i = 0; i + 2 < count; i++)
            if (i & 1)
                emit_tri(e, V(i + 1), V(i), V(i + 2));
            else
                emit_tri(e, V(i), V(i + 1), V(i + 2));
        break;
    case GL_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++)
            emit_tri(e, V(0), V(i), V(i + 1));
        break;
    case GL_POINTS:
        gc->cull = 0;
        for (i = 0; i < count; i++)
            emit_point(e, V(i), gc->point_size);
        break;
    case GL_LINES:
        gc->cull = 0;
        for (i = 0; i + 1 < count; i += 2)
            emit_line(e, V(i), V(i + 1));
        break;
    case GL_LINE_STRIP: case GL_LINE_LOOP:
        gc->cull = 0;
        for (i = 0; i + 1 < count; i++)
            emit_line(e, V(i), V(i + 1));
        if (mode == GL_LINE_LOOP && count > 2)
            emit_line(e, V(count - 1), V(0));
        break;
    }
    emit_flush(e);
    gc->cull = saved_cull;
#undef V
}

/* OES_draw_texture: a window-aligned rectangle textured with each unit's crop rectangle. */
void gl_draw_tex(GC *gc, float x, float y, float z, float w, float h)
{
    RT rt;
    Vtx q[6], c[4];
    int i, u, saved_cull = gc->cull, saved_vp[4];
    float zn;
    if (w <= 0 || h <= 0) {
        gl_set_error(gc, GL_INVALID_VALUE);
        return;
    }
    if (!gl_get_rt(gc, &rt)) {
        gl_set_error(gc, GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }
    zn = z <= 0 ? -1 : z >= 1 ? 1 : z * 2 - 1;
    memcpy(saved_vp, gc->vp, sizeof saved_vp);
    gc->vp[0] = gc->vp[1] = 0;
    gc->vp[2] = rt.w;
    gc->vp[3] = rt.h;
    for (i = 0; i < 4; i++) {
        int right = i == 1 || i == 2, top = i >= 2;
        memset(&c[i], 0, sizeof c[i]);
        c[i].x = (x + (right ? w : 0)) / rt.w * 2 - 1;
        c[i].y = (y + (top ? h : 0)) / rt.h * 2 - 1;
        c[i].z = zn;
        c[i].w = 1;
        c[i].fog = 1;
        memcpy(c[i].c, gc->color, sizeof c[i].c);
        for (u = 0; u < MAX_TEX_UNITS; u++) {
            Tex *t = gc->tu[u].enabled ? gl_bound_tex(gc, u) : NULL;
            if (!t || !t->w || !t->h)
                continue;
            c[i].t[u][0] = (t->crop[0] + (right ? t->crop[2] : 0)) / (float)t->w;
            c[i].t[u][1] = (t->crop[1] + (top ? t->crop[3] : 0)) / (float)t->h;
        }
    }
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
    q[3] = c[0]; q[4] = c[2]; q[5] = c[3];
    gc->cull = 0;
    gl_be_triangles(gc, &rt, q, 6, stereo_prepare(gc, &rt, q, 6));
    gc->cull = saved_cull;
    memcpy(gc->vp, saved_vp, sizeof saved_vp);
}
