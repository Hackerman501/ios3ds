/*
 * ios3ds-pica: PICA200 register map (the subset the GLES 1.1 back end uses).
 *
 * Two register spaces:
 *   - "GX" MMIO at PA 0x10400000: memory fill (PSC), display controllers
 *     (PDC), transfer engine (PPF) and, from +0x1000, the 3D core (P3D).
 *   - P3D "internal" registers 0x000-0x2FF. Each is one 32-bit word at
 *     GX + 0x1000 + 4*index, and is also what a command list writes.
 *
 * Values are hardware facts; the names follow libctru's registers.h and
 * libn3ds' gx.h / gpu_regs.h. See docs/pica200.md for sources.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

/* ---- physical addresses ---- */
#define PICA_GX_PA            0x10400000u   /* GX register block, 0x2000 bytes */
#define PICA_GX_SIZE          0x2000u
#define PICA_PDN_GPU_CNT_PA   0x10141200u   /* reset / clock gate */
#define PICA_CFG11_GPUPROT_PA 0x10140140u   /* which RAM the GPU may DMA */
#define PICA_VRAM_PA          0x18000000u
#define PICA_VRAM_SIZE        0x00600000u

/* PDN_GPU_CNT */
#define PDN_GPU_CNT_NORST_ALL 0x7Fu
#define PDN_GPU_CNT_CLK_EN    (1u << 16)

/* GIC interrupt IDs */
#define PICA_IRQ_PSC0 40
#define PICA_IRQ_PSC1 41
#define PICA_IRQ_PDC0 42
#define PICA_IRQ_PDC1 43
#define PICA_IRQ_PPF  44
#define PICA_IRQ_P3D  45

/* ---- GX MMIO, as u32 word indices from PICA_GX_PA ---- */
#define GX_GPU_CLK        (0x004 / 4)
#define GX_PSC_FILL0      (0x010 / 4)   /* start>>3, end>>3, value, control */
#define GX_PSC_FILL1      (0x020 / 4)
#define GX_PSC_VRAM       (0x030 / 4)   /* bits 8-11: VRAM bank disable */
#define GX_PSC_IRQ_STAT   (0x034 / 4)
#define GX_PSC_DMA_PRIO0  (0x050 / 4)
#define GX_PSC_DMA_PRIO1  (0x054 / 4)
#define GX_PPF_IN_ADDR    (0xC00 / 4)   /* >>3 */
#define GX_PPF_OUT_ADDR   (0xC04 / 4)   /* >>3 */
#define GX_PPF_DT_OUTDIM  (0xC08 / 4)   /* h<<16 | w */
#define GX_PPF_DT_INDIM   (0xC0C / 4)
#define GX_PPF_FLAGS      (0xC10 / 4)
#define GX_PPF_UNK14      (0xC14 / 4)
#define GX_PPF_CNT        (0xC18 / 4)
#define GX_PPF_TC_LEN     (0xC20 / 4)   /* texture copy: bytes */
#define GX_PPF_TC_INDIM   (0xC24 / 4)   /* gap<<16 | line width, 16-byte units */
#define GX_PPF_TC_OUTDIM  (0xC28 / 4)
#define GX_P3D            (0x1000 / 4)  /* + P3D register index */

/* PSC fill control */
#define PSC_FILL_START    (1u << 0)
#define PSC_FILL_DONE     (1u << 1)
#define PSC_FILL_16       (0u << 8)
#define PSC_FILL_24       (1u << 8)
#define PSC_FILL_32       (2u << 8)

/* GX_PSC_IRQ_STAT */
#define GX_STAT_PSC0      (1u << 26)
#define GX_STAT_PSC1      (1u << 27)
#define GX_STAT_PDC0      (1u << 28)
#define GX_STAT_PDC1      (1u << 29)
#define GX_STAT_PPF       (1u << 30)
#define GX_STAT_P3D       (1u << 31)

/* PPF flags / control */
#define PPF_FLIP_V        (1u << 0)
#define PPF_OUT_TILED     (1u << 1)     /* linear -> 8x8 tiled (texture upload) */
#define PPF_CROP          (1u << 2)
#define PPF_TEXCOPY       (1u << 3)     /* raw copy using TC_* registers */
#define PPF_RAW           (1u << 5)     /* with OUT linear: no tiled->linear step */
#define PPF_IN_FMT(f)     ((unsigned)(f) << 8)
#define PPF_OUT_FMT(f)    ((unsigned)(f) << 12)
#define PPF_TILE32        (1u << 16)
#define PPF_SCALE_X       (1u << 24)    /* 2x1 box filter */
#define PPF_SCALE_XY      (2u << 24)    /* 2x2 box filter */
#define PPF_CNT_START     (1u << 0)
#define PPF_CNT_DONE      (1u << 8)

/* PPF (display transfer) pixel formats (bytes low -> high address) */
#define PICA_FB_RGBA8     0   /* A, B, G, R  (u32 = R<<24 | G<<16 | B<<8 | A) */
#define PICA_FB_RGB8      1   /* B, G, R */
#define PICA_FB_RGB565    2   /* u16 = R5 G6 B5 */
#define PICA_FB_RGB5A1    3   /* u16 = R5 G5 B5 A1 */
#define PICA_FB_RGBA4     4   /* u16 = R4 G4 B4 A4 */

/* Colour buffer formats (COLORBUFFER_FORMAT): the two 16-bit RGB formats
   are numbered the other way round than for PPF. */
#define PICA_CB_RGBA8     0
#define PICA_CB_RGB8      1
#define PICA_CB_RGB5A1    2
#define PICA_CB_RGB565    3
#define PICA_CB_RGBA4     4
/* depth buffer formats */
#define PICA_DEPTH16      0
#define PICA_DEPTH24      2
#define PICA_DEPTH24S8    3

/* ---- P3D internal registers ---- */
#define P3D_IRQ_ACK                 0x000
#define P3D_FINALIZE                0x010   /* write == IRQ_CMP -> P3D IRQ */
#define P3D_IRQ_CMP                 0x020
#define P3D_IRQ_MASK                0x030
#define P3D_IRQ_AUTOSTOP            0x034

#define P3D_FACECULLING_CONFIG      0x040
#define P3D_VIEWPORT_WIDTH          0x041   /* f24 w/2 */
#define P3D_VIEWPORT_INVW           0x042   /* f31(2/w) << 1 */
#define P3D_VIEWPORT_HEIGHT         0x043
#define P3D_VIEWPORT_INVH           0x044
#define P3D_DEPTHMAP_SCALE          0x04D   /* f24 */
#define P3D_DEPTHMAP_OFFSET         0x04E   /* f24 */
#define P3D_SH_OUTMAP_TOTAL         0x04F
#define P3D_SH_OUTMAP_O0            0x050   /* .. O6 = 0x056 */
#define P3D_EARLYDEPTH_FUNC         0x061
#define P3D_EARLYDEPTH_TEST1        0x062
#define P3D_EARLYDEPTH_CLEAR        0x063
#define P3D_SH_OUTATTR_MODE         0x064
#define P3D_SCISSORTEST_MODE        0x065
#define P3D_SCISSORTEST_POS         0x066   /* y1<<16 | x1 */
#define P3D_SCISSORTEST_DIM         0x067   /* (y2-1)<<16 | (x2-1) */
#define P3D_VIEWPORT_XY             0x068   /* y<<16 | x */
#define P3D_EARLYDEPTH_DATA         0x06A
#define P3D_DEPTHMAP_ENABLE         0x06D
#define P3D_RENDERBUF_DIM           0x06E
#define P3D_SH_OUTATTR_CLOCK        0x06F

#define P3D_TEXUNIT_CONFIG          0x080
#define P3D_TEXUNIT0_BORDER_COLOR   0x081   /* +1 DIM, +2 PARAM, +3 LOD, +4 ADDR */
#define P3D_TEXUNIT0_SHADOW         0x08B
#define P3D_TEXUNIT0_TYPE           0x08E
#define P3D_LIGHTING_ENABLE0        0x08F
#define P3D_TEXUNIT1_BORDER_COLOR   0x091
#define P3D_TEXUNIT1_TYPE           0x096
#define P3D_TEXUNIT2_BORDER_COLOR   0x099
#define P3D_TEXUNIT2_TYPE           0x09E
#define P3D_TEXENV0_SOURCE          0x0C0   /* +1 OPERAND +2 COMBINER +3 COLOR +4 SCALE */
#define P3D_TEXENV1_SOURCE          0x0C8
#define P3D_TEXENV2_SOURCE          0x0D0
#define P3D_TEXENV3_SOURCE          0x0D8
#define P3D_TEXENV_UPDATE_BUFFER    0x0E0
#define P3D_FOG_COLOR               0x0E1
#define P3D_TEXENV4_SOURCE          0x0F0
#define P3D_TEXENV5_SOURCE          0x0F8
#define P3D_TEXENV_BUFFER_COLOR     0x0FD

#define P3D_COLOR_OPERATION         0x100
#define P3D_BLEND_FUNC              0x101
#define P3D_LOGIC_OP                0x102
#define P3D_BLEND_COLOR             0x103
#define P3D_FRAGOP_ALPHA_TEST       0x104
#define P3D_STENCIL_TEST            0x105
#define P3D_STENCIL_OP              0x106
#define P3D_DEPTH_COLOR_MASK        0x107
#define P3D_FRAMEBUFFER_INVALIDATE  0x110
#define P3D_FRAMEBUFFER_FLUSH       0x111
#define P3D_COLORBUFFER_READ        0x112
#define P3D_COLORBUFFER_WRITE       0x113
#define P3D_DEPTHBUFFER_READ        0x114
#define P3D_DEPTHBUFFER_WRITE       0x115
#define P3D_DEPTHBUFFER_FORMAT      0x116
#define P3D_COLORBUFFER_FORMAT      0x117
#define P3D_EARLYDEPTH_TEST2        0x118
#define P3D_FRAMEBUFFER_BLOCK32     0x11B
#define P3D_DEPTHBUFFER_LOC         0x11C   /* PA >> 3 */
#define P3D_COLORBUFFER_LOC         0x11D   /* PA >> 3 */
#define P3D_FRAMEBUFFER_DIM         0x11E
#define P3D_GAS_DELTAZ_DEPTH        0x126
#define P3D_FRAGOP_SHADOW           0x130
#define P3D_LIGHTING_ENABLE1        0x1C6

#define P3D_ATTRIBBUFFERS_LOC       0x200   /* base PA >> 3 */
#define P3D_ATTRIBBUFFERS_FORMAT_LOW  0x201
#define P3D_ATTRIBBUFFERS_FORMAT_HIGH 0x202
#define P3D_ATTRIBBUFFER0_OFFSET    0x203   /* 12 x {OFFSET, CONFIG1, CONFIG2} */
#define P3D_INDEXBUFFER_CONFIG      0x227
#define P3D_NUMVERTICES             0x228
#define P3D_GEOSTAGE_CONFIG         0x229
#define P3D_VERTEX_OFFSET           0x22A
#define P3D_POST_VERTEX_CACHE_NUM   0x22D
#define P3D_DRAWARRAYS              0x22E
#define P3D_DRAWELEMENTS            0x22F
#define P3D_VTX_FUNC                0x231
#define P3D_FIXEDATTRIB_INDEX       0x232
#define P3D_FIXEDATTRIB_DATA0       0x233
#define P3D_CMDBUF_SIZE0            0x238   /* bytes >> 3 */
#define P3D_CMDBUF_SIZE1            0x239
#define P3D_CMDBUF_ADDR0            0x23A   /* PA >> 3 */
#define P3D_CMDBUF_ADDR1            0x23B
#define P3D_CMDBUF_JUMP0            0x23C
#define P3D_CMDBUF_JUMP1            0x23D
#define P3D_VSH_NUM_ATTR            0x242
#define P3D_VSH_COM_MODE            0x244
#define P3D_START_DRAW_FUNC0        0x245   /* 1 = configuration mode, 0 = draw mode */
#define P3D_VSH_OUTMAP_TOTAL1       0x24A
#define P3D_VSH_OUTMAP_TOTAL2       0x251
#define P3D_GSH_MISC0               0x252
#define P3D_GEOSTAGE_CONFIG2        0x253
#define P3D_GSH_MISC1               0x254
#define P3D_PRIMITIVE_CONFIG        0x25E
#define P3D_RESTART_PRIMITIVE       0x25F
#define P3D_GSH_INPUTBUFFER_CONFIG  0x289
#define P3D_VSH_BOOLUNIFORM         0x2B0
#define P3D_VSH_INTUNIFORM_I0       0x2B1
#define P3D_VSH_INPUTBUFFER_CONFIG  0x2B9
#define P3D_VSH_ENTRYPOINT          0x2BA
#define P3D_VSH_ATTR_PERM_LOW       0x2BB
#define P3D_VSH_ATTR_PERM_HIGH      0x2BC
#define P3D_VSH_OUTMAP_MASK         0x2BD
#define P3D_VSH_CODETRANSFER_END    0x2BF
#define P3D_VSH_FLOATUNIFORM_CONFIG 0x2C0   /* bit31: data is 4 x f32 (w,z,y,x) */
#define P3D_VSH_FLOATUNIFORM_DATA   0x2C1
#define P3D_VSH_CODETRANSFER_CONFIG 0x2CB
#define P3D_VSH_CODETRANSFER_DATA   0x2CC
#define P3D_VSH_OPDESCS_CONFIG      0x2D5
#define P3D_VSH_OPDESCS_DATA        0x2D6

/* value written to FINALIZE / IRQ_CMP to end a command list */
#define PICA_FINALIZE_MAGIC 0x12345678u

/* ---- field enums ---- */
enum { PICA_CULL_NONE = 0, PICA_CULL_FRONT_CCW = 1, PICA_CULL_BACK_CCW = 2 };
enum { PICA_NEVER, PICA_ALWAYS, PICA_EQUAL, PICA_NOTEQUAL, PICA_LESS, PICA_LEQUAL, PICA_GREATER, PICA_GEQUAL };
enum { PICA_BLEND_ADD, PICA_BLEND_SUB, PICA_BLEND_RSUB, PICA_BLEND_MIN, PICA_BLEND_MAX };
enum {
    PICA_BF_ZERO, PICA_BF_ONE, PICA_BF_SRC_COLOR, PICA_BF_INV_SRC_COLOR, PICA_BF_DST_COLOR,
    PICA_BF_INV_DST_COLOR, PICA_BF_SRC_ALPHA, PICA_BF_INV_SRC_ALPHA, PICA_BF_DST_ALPHA,
    PICA_BF_INV_DST_ALPHA, PICA_BF_CONST_COLOR, PICA_BF_INV_CONST_COLOR, PICA_BF_CONST_ALPHA,
    PICA_BF_INV_CONST_ALPHA, PICA_BF_SRC_ALPHA_SAT
};
enum { PICA_STENCIL_KEEP, PICA_STENCIL_ZERO, PICA_STENCIL_REPLACE, PICA_STENCIL_INCR,
       PICA_STENCIL_DECR, PICA_STENCIL_INVERT, PICA_STENCIL_INCR_WRAP, PICA_STENCIL_DECR_WRAP };
enum { PICA_WRITE_R = 1, PICA_WRITE_G = 2, PICA_WRITE_B = 4, PICA_WRITE_A = 8, PICA_WRITE_DEPTH = 16,
       PICA_WRITE_COLOR = 15, PICA_WRITE_ALL = 31 };
enum { PICA_SCISSOR_OFF = 0, PICA_SCISSOR_INVERT = 1, PICA_SCISSOR_ON = 3 };

/* vertex attribute component types (never normalized: bytes arrive as 0..255) */
enum { PICA_BYTE = 0, PICA_UBYTE = 1, PICA_SHORT = 2, PICA_FLOAT = 3 };

enum { PICA_TRIANGLES = 0x000, PICA_TRIANGLE_STRIP = 0x100, PICA_TRIANGLE_FAN = 0x200,
       PICA_GEOMETRY_PRIM = 0x300 };

/* texture formats (TEXUNITn_TYPE); texels are 8x8-tiled, see pica_tex.c */
enum {
    PICA_TEX_RGBA8 = 0,    /* A, B, G, R */
    PICA_TEX_RGB8 = 1,     /* B, G, R */
    PICA_TEX_RGBA5551 = 2, /* u16 R5 G5 B5 A1 */
    PICA_TEX_RGB565 = 3,   /* u16 R5 G6 B5 */
    PICA_TEX_RGBA4 = 4,    /* u16 R4 G4 B4 A4 */
    PICA_TEX_LA8 = 5,      /* A, L */
    PICA_TEX_HILO8 = 6,
    PICA_TEX_L8 = 7,
    PICA_TEX_A8 = 8,
    PICA_TEX_LA4 = 9,      /* L in the high nibble */
    PICA_TEX_L4 = 10,      /* two texels per byte, low nibble first */
    PICA_TEX_A4 = 11,
    PICA_TEX_ETC1 = 12,
    PICA_TEX_ETC1A4 = 13
};
enum { PICA_NEAREST = 0, PICA_LINEAR = 1 };
enum { PICA_CLAMP_TO_EDGE = 0, PICA_CLAMP_TO_BORDER = 1, PICA_REPEAT = 2, PICA_MIRRORED_REPEAT = 3 };

/* TexEnv ("TEV") stage fields */
enum { PICA_TEVSRC_PRIMARY = 0, PICA_TEVSRC_FRAG_PRIMARY = 1, PICA_TEVSRC_FRAG_SECONDARY = 2,
       PICA_TEVSRC_TEXTURE0 = 3, PICA_TEVSRC_TEXTURE1 = 4, PICA_TEVSRC_TEXTURE2 = 5,
       PICA_TEVSRC_PREV_BUFFER = 13, PICA_TEVSRC_CONSTANT = 14, PICA_TEVSRC_PREVIOUS = 15 };
enum { PICA_TEVOP_RGB_COLOR = 0, PICA_TEVOP_RGB_INV_COLOR = 1, PICA_TEVOP_RGB_ALPHA = 2,
       PICA_TEVOP_RGB_INV_ALPHA = 3, PICA_TEVOP_RGB_R = 4, PICA_TEVOP_RGB_INV_R = 5,
       PICA_TEVOP_RGB_G = 8, PICA_TEVOP_RGB_INV_G = 9, PICA_TEVOP_RGB_B = 12, PICA_TEVOP_RGB_INV_B = 13 };
enum { PICA_TEVOP_A_ALPHA = 0, PICA_TEVOP_A_INV_ALPHA = 1, PICA_TEVOP_A_R = 2, PICA_TEVOP_A_INV_R = 3,
       PICA_TEVOP_A_G = 4, PICA_TEVOP_A_INV_G = 5, PICA_TEVOP_A_B = 6, PICA_TEVOP_A_INV_B = 7 };
enum { PICA_TEV_REPLACE = 0, PICA_TEV_MODULATE = 1, PICA_TEV_ADD = 2, PICA_TEV_ADD_SIGNED = 3,
       PICA_TEV_INTERPOLATE = 4, PICA_TEV_SUBTRACT = 5, PICA_TEV_DOT3_RGB = 6, PICA_TEV_DOT3_RGBA = 7,
       PICA_TEV_MULT_ADD = 8, PICA_TEV_ADD_MULT = 9 };

/* shader output semantics, one byte per component in SH_OUTMAP_On */
enum { PICA_SEM_POS_X = 0x00, PICA_SEM_COLOR_R = 0x08, PICA_SEM_TC0_U = 0x0C, PICA_SEM_TC1_U = 0x0E,
       PICA_SEM_TC0_W = 0x10, PICA_SEM_TC2_U = 0x16, PICA_SEM_UNUSED = 0x1F };
