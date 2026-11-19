/*
 * ios3ds-pica: PICA200 back end for the ios3ds OpenGL ES 1.1 engine.
 *
 * Layers, bottom up (each usable and testable without the ones above):
 *   pica_cmd_*    command list encoder                      src/pica_cmd.c
 *   pica_emit_*   register-level state encoders             src/pica_state.c
 *   pica_tex_*    texel conversion + 8x8 Morton tiling      src/pica_tex.c
 *   pica_ff_*     fixed-function emulation (vertex shader,
 *                 GL texenv -> TEV stages, GL enum mapping) src/pica_ff.c
 * The GPU itself is reached through pica_dev.h (sysctl to the kernel shim, or
 * the software model on the host); the register-level driver is pica_drv.c.
 *
 * Nothing here allocates or keeps global state; all "colour" words are
 * R | G<<8 | B<<16 | A<<24.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "pica_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Command lists                                                       */
/* ------------------------------------------------------------------ */

/* A command list is a sequence of {parameter, header[, more parameters]}
   groups of 32-bit little-endian words, each group padded to 8 bytes.
   The buffer must be GPU-visible (arena) memory, 16-byte aligned. */
typedef struct {
    uint32_t *buf;
    uint32_t  cap;      /* words */
    uint32_t  len;      /* words written */
    int       overflow; /* set instead of writing past cap; the list is then unusable */
} pica_cmd_t;

void pica_cmd_init(pica_cmd_t *c, uint32_t *buf, uint32_t cap_words);
/* reg = value */
void pica_cmd_write(pica_cmd_t *c, uint32_t reg, uint32_t value);
/* Update only the bytes of reg selected by the 4-bit byte mask. */
void pica_cmd_write_masked(pica_cmd_t *c, uint32_t reg, uint32_t bytemask, uint32_t value);
/* reg, reg+1, ... reg+n-1 = vals[0..n-1] */
void pica_cmd_write_seq(pica_cmd_t *c, uint32_t reg, const uint32_t *vals, uint32_t n);
/* n writes to the same register (FIFO-style registers: shader code, uniforms) */
void pica_cmd_write_burst(pica_cmd_t *c, uint32_t reg, const uint32_t *vals, uint32_t n);
/* Append the end marker and pad to 16 bytes. Returns the size in bytes to
   hand to pica_dev_t.submit, or 0 if the list overflowed. */
uint32_t pica_cmd_finish(pica_cmd_t *c);

/* IEEE float -> the GPU's 16/24/31-bit float formats */
uint32_t pica_f16(float f);
uint32_t pica_f24(float f);
uint32_t pica_f31(float f);

/* ------------------------------------------------------------------ */
/* State encoders                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t color_pa;      /* 0 = no colour buffer */
    uint32_t depth_pa;      /* 0 = no depth buffer */
    uint16_t width, height; /* pixels; buffers are 8x8-tiled like textures */
    uint8_t  color_fmt;     /* PICA_CB_* */
    uint8_t  depth_fmt;     /* PICA_DEPTH* */
} pica_fb_t;

/* Bytes per pixel of a colour / depth buffer format. */
uint32_t pica_fb_color_bpp(unsigned fmt);
uint32_t pica_fb_depth_bpp(unsigned fmt);

/* Bind a render target. Call pica_emit_fb_flush first if anything was drawn
   to the previous one in this list. */
void pica_emit_framebuffer(pica_cmd_t *c, const pica_fb_t *fb);
void pica_emit_fb_flush(pica_cmd_t *c);
void pica_emit_viewport(pica_cmd_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* GL-style box (origin bottom-left). enable = 0 turns the test off. */
void pica_emit_scissor(pica_cmd_t *c, int enable, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

typedef struct {
    uint8_t  cull;              /* PICA_CULL_* */
    uint8_t  depth_test;        /* bool */
    uint8_t  depth_func;        /* PICA_LESS ... */
    uint8_t  write_mask;        /* PICA_WRITE_* */
    uint8_t  alpha_test, alpha_func, alpha_ref;
    uint8_t  stencil_test, stencil_func, stencil_ref, stencil_mask, stencil_write_mask;
    uint8_t  stencil_sfail, stencil_zfail, stencil_zpass;
    uint8_t  blend;             /* bool; when 0 and logic_op_enable 0 the fragment replaces the pixel */
    uint8_t  blend_eq_rgb, blend_eq_a;
    uint8_t  blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    uint8_t  logic_op_enable, logic_op;
    uint32_t blend_color;
    float    depth_near, depth_far;     /* glDepthRange */
    float    depth_bias;                /* glPolygonOffset, already in depth units */
    uint8_t  depth_conv;                /* PICA_Z_*: how GL depth is stored (docs/pica200.md 16) */
} pica_raster_t;

/* How GL window depth d (glDepthRange, glDepthFunc, glClearDepth semantics)
   becomes the stored 24-bit value. Hardware run 15: with LEGACY (stored = d,
   a clear to 1.0 lands exactly on 1.0) nothing passed a depth test.
   REVERSED is citro3d's convention (C3D_DepthMap(true, -1, 0), GPU_GREATER):
   stored = K * (1 - d), comparisons mirrored, far = 0. STANDARD keeps
   stored = K * d. K = 1 - 2^-10 keeps every stored value below 1.0. */
enum { PICA_Z_REVERSED = 0, PICA_Z_STANDARD = 1, PICA_Z_LEGACY = 2 };
#define PICA_Z_K (1.0f - 1.0f / 1024.0f)
/* the stored 24-bit value for GL depth d under a convention (CPU-side initialisation) */
uint32_t pica_depth_stored(unsigned conv, float d);
/* the GPU test function for a GL one under a convention */
unsigned pica_depth_func(unsigned conv, unsigned func);

/* GL defaults: no cull, no tests, write everything, range 0..1. */
void pica_raster_defaults(pica_raster_t *r);
void pica_emit_raster(pica_cmd_t *c, const pica_raster_t *r);

typedef struct {
    uint32_t pa;                /* tiled texel data, 8-byte aligned */
    uint16_t width, height;     /* powers of two, 8..1024 */
    uint8_t  fmt;               /* PICA_TEX_* */
    uint8_t  mag, min;          /* PICA_NEAREST / PICA_LINEAR */
    uint8_t  wrap_s, wrap_t;    /* PICA_CLAMP_TO_EDGE ... */
    uint32_t border;            /* colour */
} pica_tex_t;

void pica_emit_texture(pica_cmd_t *c, unsigned unit, const pica_tex_t *t);
/* Which of units 0..2 are sampled (bit per unit). Also drops the GPU's
   texture cache, which is required after changing texel data. */
void pica_emit_texunits(pica_cmd_t *c, unsigned unit_mask);

typedef struct {
    uint8_t  src_rgb[3], src_a[3];  /* PICA_TEVSRC_* */
    uint8_t  op_rgb[3], op_a[3];    /* PICA_TEVOP_RGB_* / PICA_TEVOP_A_* */
    uint8_t  func_rgb, func_a;      /* PICA_TEV_* */
    uint8_t  scale_rgb, scale_a;    /* 0, 1, 2 = x1, x2, x4 */
    uint32_t color;                 /* PICA_TEVSRC_CONSTANT */
} pica_tev_t;

#define PICA_TEV_STAGES 6
/* stage := previous (the pass-through every unused stage must be set to) */
void pica_tev_passthrough(pica_tev_t *t);
void pica_emit_tev(pica_cmd_t *c, unsigned stage, const pica_tev_t *t);

/* Vertex input: up to 12 attributes, each either read from an interleaved
   array or constant for the whole draw. Attribute i feeds shader input
   register v<i>. */
typedef struct {
    uint8_t  count;             /* attributes */
    struct {
        uint8_t type;           /* PICA_BYTE ... PICA_FLOAT */
        uint8_t size;           /* 1..4 components */
        uint8_t fixed;          /* 1 = constant, set with pica_emit_fixed_attr; no array */
        uint8_t buffer;         /* which array it comes from */
    } attr[12];
    uint8_t  nbuffers;
    struct {
        uint32_t offset;        /* bytes from base_pa to the first vertex */
        uint8_t  stride;        /* bytes */
    } buffer[12];
    uint32_t base_pa;           /* all arrays and index data: base_pa + offset; 8-byte aligned */
} pica_vtx_layout_t;

/* Attributes of one buffer must be listed in the order they are stored in a
   vertex, tightly packed. Returns 0, or -1 for a layout the GPU cannot take. */
int  pica_emit_vtx_layout(pica_cmd_t *c, const pica_vtx_layout_t *l);
void pica_emit_fixed_attr(pica_cmd_t *c, unsigned index, const float v[4]);

void pica_emit_draw_arrays(pica_cmd_t *c, unsigned prim, uint32_t first, uint32_t count);
/* index_off: bytes from the layout's base_pa. is16: 0 = u8 indices, 1 = u16. */
void pica_emit_draw_elements(pica_cmd_t *c, unsigned prim, uint32_t count, uint32_t index_off, int is16);

typedef struct {
    const uint32_t *code;   uint32_t ncode;
    const uint32_t *opdesc; uint32_t nopdesc;
    uint32_t entry;                 /* instruction index of main */
    uint32_t outmap_total;          /* output registers used */
    uint32_t outmap_mask;           /* bit per output register */
    uint32_t outmap[7];             /* semantic per component, PICA_SEM_* */
    uint32_t outattr_mode;          /* SH_OUTATTR_MODE: 1 if any texcoord output */
    uint32_t outattr_clock;         /* SH_OUTATTR_CLOCK */
} pica_shader_t;

/* Upload a vertex shader and select it (no geometry shader). */
void pica_emit_shader(pica_cmd_t *c, const pica_shader_t *s);
/* c[first..first+n-1] = v[i] = {x, y, z, w} */
void pica_emit_uniforms(pica_cmd_t *c, unsigned first, const float (*v)[4], unsigned n);

/* Everything a fresh command list must set that the encoders above do not:
   no geometry shader, no lighting, no fog, all TEV stages pass-through. */
void pica_emit_base_state(pica_cmd_t *c);

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

/* Client pixel layouts (bytes in memory order) */
typedef enum {
    PICA_SRC_RGBA8888,      /* R, G, B, A   GL_RGBA / GL_UNSIGNED_BYTE */
    PICA_SRC_BGRA8888,      /* B, G, R, A   GL_BGRA, CoreGraphics / IOSurface 'BGRA' */
    PICA_SRC_RGB888,        /* R, G, B */
    PICA_SRC_RGB565,        /* native u16 */
    PICA_SRC_RGBA4444,      /* native u16 */
    PICA_SRC_RGBA5551,      /* native u16 */
    PICA_SRC_L8, PICA_SRC_A8,
    PICA_SRC_LA88,          /* L, A */
} pica_srcfmt_t;

uint32_t pica_tex_bpp(unsigned fmt);                    /* bits per texel; 0 = unsupported */
uint32_t pica_tex_bytes(unsigned fmt, uint32_t w, uint32_t h);
uint32_t pica_pot(uint32_t n);                          /* next power of two, at least 8 */
/* Offset of texel (x, y) in texels, y counted from the first stored row. */
uint32_t pica_tex_index(uint32_t x, uint32_t y, uint32_t w);
/* The texture format that holds a client format without loss. */
unsigned pica_tex_fmt_for(pica_srcfmt_t sfmt);

/*
 * Convert and tile client pixels into texture memory.
 *   dst, dw x dh   texture of format fmt (dims are powers of two >= 8)
 *   dx, dy         where the sw x sh source rectangle goes, in GL texel
 *                  coordinates (t = 0 is the first source row)
 *   stride         source bytes per row
 * The source may be smaller than the texture (NPOT content in a POT texture,
 * glTexSubImage2D); texels outside the rectangle are left alone.
 * GL's first row is t = 0 and the GPU's first stored row is t = 1, so rows
 * are stored in reverse.
 * Returns 0, or -1 for an unsupported format pair / out-of-range rectangle.
 */
int pica_tex_upload(void *dst, unsigned fmt, uint32_t dw, uint32_t dh,
                    uint32_t dx, uint32_t dy,
                    const void *src, pica_srcfmt_t sfmt, uint32_t sw, uint32_t sh, uint32_t stride);

/* Replicate the edge texels of a w x h sub-rectangle at (0,0) one texel
   outwards so bilinear filtering at the edge of NPOT content stays clean. */
void pica_tex_pad_edges(void *dst, unsigned fmt, uint32_t dw, uint32_t dh, uint32_t w, uint32_t h);

/* CPU read-back of a tiled RGBA8 colour buffer into top-down BGRA (the
   layout of the iOS framebuffer and of IOSurfaces). Only the w x h window
   whose GL origin is (x, y) is copied. Slow path (the back end uses a PPF display transfer). */
void pica_untile_rgba8_to_bgra(void *dst, uint32_t dst_stride,
                               const void *fb, uint32_t fb_w, uint32_t fb_h,
                               uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* ------------------------------------------------------------------ */
/* Fixed-function emulation                                            */
/* ------------------------------------------------------------------ */

/* Shader interface (shader/ff.vsh):
     v0 position   v1 colour   v2 texcoord 0   v3 texcoord 1
     c0-c3  rows of the clip matrix       c4,c5  rows s,t of texture matrix 0
     c6,c7  rows s,t of texture matrix 1  c8     colour scale
     c9     (0, 0, 0, 1)                                                */
enum { PICA_FF_POSITION = 0, PICA_FF_COLOR = 1, PICA_FF_TEXCOORD0 = 2, PICA_FF_TEXCOORD1 = 3,
       PICA_FF_ATTRS = 4 };

extern const pica_shader_t pica_ff_shader;

/* Shader + constant uniforms. Once per command list that draws. */
void pica_ff_emit_program(pica_cmd_t *c);

/* clip = fix * projection * modelview, where fix maps GL clip space
   (-w <= z <= w) to the GPU's (-w <= z <= 0). Matrices are GL style:
   column-major float[16]. */
void pica_ff_clip_matrix(float out[16], const float projection[16], const float modelview[16]);

/* Upload the per-draw uniforms. tex0 / tex1 may be NULL (identity).
   color_scale multiplies the colour attribute: 1/255 for GL_UNSIGNED_BYTE
   colour arrays (attributes are never normalized), 1 for floats and for the
   constant glColor. */
void pica_ff_emit_matrices(pica_cmd_t *c, const float clip[16],
                           const float tex0[16], const float tex1[16], float color_scale);

/* Texture matrix that also rescales [0,1] over w x h content to the corner
   of a tw x th texture (NPOT emulation). m may be NULL (identity). */
void pica_ff_tex_matrix(float out[16], const float m[16], uint32_t w, uint32_t h, uint32_t tw, uint32_t th);

/* One GL texture unit's glTexEnv state. */
typedef struct {
    uint32_t mode;          /* GL_MODULATE, GL_REPLACE, GL_DECAL, GL_BLEND, GL_ADD */
    uint32_t base_format;   /* GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_BGRA */
    uint32_t env_color;     /* GL_TEXTURE_ENV_COLOR */
    /* mode == GL_COMBINE only: */
    uint32_t comb_rgb, comb_a;          /* GL_REPLACE ... GL_DOT3_RGBA */
    uint32_t src_rgb[3], src_a[3];      /* GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS */
    uint32_t op_rgb[3], op_a[3];        /* GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA */
    uint8_t  scale_rgb, scale_a;        /* 1, 2 or 4 */
} pica_gl_texenv_t;

/* Fill TEV stages for the enabled GL units (unit i samples PICA texture unit
   i). units[i] == NULL = texturing disabled on that unit. Unused stages
   become pass-through. Returns the number of stages used, or -1 for an
   unknown mode / combine function. */
int pica_ff_texenv(pica_tev_t out[PICA_TEV_STAGES], const pica_gl_texenv_t *units[2]);

/* TEV program for the present blit: outputs the sampled texture with its
   channels rotated so that an RGBA8 colour buffer holds B,G,R,A bytes, the
   layout of the iOS framebuffer. Uses stages 0..2. */
void pica_ff_tev_bgra_swizzle(pica_tev_t out[PICA_TEV_STAGES]);

/* GL enum -> PICA field. Return -1 for values the GPU has no equivalent of. */
int pica_gl_func(uint32_t gl_func);             /* GL_NEVER ... GL_ALWAYS */
int pica_gl_blend_factor(uint32_t gl_factor);
int pica_gl_blend_eq(uint32_t gl_eq);
int pica_gl_stencil_op(uint32_t gl_op);
int pica_gl_logic_op(uint32_t gl_op);
int pica_gl_wrap(uint32_t gl_wrap);
int pica_gl_prim(uint32_t gl_mode);             /* triangles, strips and fans only */
int pica_gl_attr_type(uint32_t gl_type);        /* GL_FIXED and unsigned short -> -1: convert on the CPU */

#ifdef __cplusplus
}
#endif
