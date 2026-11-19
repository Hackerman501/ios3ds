/*
 * The AR6014 firmware straight from NWM's .code (sd:/ios3ds/nwm.code, written
 * by tools/ios3ds-wifi/ios3ds-wifi-fw.gm9 in GodMode9), so no PC step is
 * needed. Same logic as tools/ios3ds-wifi/nwmfw.py (the host tests compare
 * both byte for byte): BLZ decompression in place, the literal pools GBATEK
 * "3DS Files - Module NWM (Wifi Driver)" describes, the fixed block sizes,
 * then the 'I3WF' container ar6k.c uploads.
 * Pure C, no hardware, no logging: host-tested by tools/ios3ds-wifi/tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"

#define MAIN_DST   0x00524C00u
#define DB_DST     0x0053FE18u
#define CODE_DST   0x00527000u
#define POOL_3ED   0x000003EDu
#define I3WF_MAGIC 0x46573349u
#define CHIP_AR6014 0x0D000001u

static u32 rd32(const u8 *c, u32 len, u32 off)
{
    if (off > len || len - off < 4) return 0;
    return (u32)c[off] | (u32)c[off + 1] << 8 | (u32)c[off + 2] << 16 | (u32)c[off + 3] << 24;
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

u32 nwm_crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* BLZ footer: u32 (bottom << 24 | top), u32 extra size (ctrtool lzss.c) */
static int blz_ok(const u8 *d, u32 len)
{
    if (len < 8) return 0;
    u32 tb = rd32(d, len, len - 8), extra = rd32(d, len, len - 4);
    u32 bottom = tb >> 24, top = tb & 0xFFFFFF;
    return bottom >= 8 && bottom <= 0x0B && bottom <= top && top <= len && extra < 0x1000000u;
}

/* In place: d holds len compressed bytes, cap >= len + extra. */
static int blz_decompress(u8 *d, u32 len, u32 cap, u32 *outlen)
{
    u32 tb = rd32(d, len, len - 8), extra = rd32(d, len, len - 4);
    u32 bottom = tb >> 24, top = tb & 0xFFFFFF;
    u32 total = len + extra;
    if (total > cap || total < len) return NWM_E_TOOBIG;
    u32 index = len - bottom, stop = len - top, o = total;
    while (index > stop) {
        u8 control = d[--index];
        for (int b = 0; b < 8; b++) {
            if (index <= stop || o == 0) break;
            if (control & 0x80) {
                if (index < 2) return NWM_E_BLZ;
                index -= 2;
                u32 seg = d[index] | (u32)d[index + 1] << 8;
                u32 size = (seg >> 12) + 3, off = (seg & 0x0FFF) + 2;
                for (u32 i = 0; i < size; i++) {
                    if (o == 0 || o + off >= total) return NWM_E_BLZ;
                    d[o - 1] = d[o + off];
                    o--;
                }
            } else {
                if (o == 0) return NWM_E_BLZ;
                d[--o] = d[--index];
            }
            control = (u8)(control << 1);
        }
    }
    *outlen = total;
    return 0;
}

/* addresses of one pool: [kind] = {start, end}; kinds as nwm_info_t */
typedef struct { u32 s[NWM_NBLK], e[NWM_NBLK]; u8 layout; } pool_t;

static int find_pool(const u8 *c, u32 len, u32 from, pool_t *p, u32 *at)
{
    for (u32 i = from; i + 4 <= len; i += 4) {
        if (rd32(c, len, i) != MAIN_DST) continue;
#define W(d) rd32(c, len, i + (d))
        memset(p, 0, sizeof(*p));
        if (W(4) == POOL_3ED && W(0x28) == DB_DST && W(0x34) == CODE_DST) {
            /* GBATEK "Newer Large Pool", main.dst at +14h */
            p->layout = NWM_NEWER;
            p->s[NWM_T1] = W(0x0C); p->e[NWM_T1] = W(0x08);
            p->s[NWM_T4] = W(0x14); p->e[NWM_T4] = W(0x10);
            p->s[NWM_T5] = W(0x1C); p->e[NWM_T5] = W(0x18);
            p->s[NWM_DB] = W(0x24); p->e[NWM_DB] = W(0x20);
            p->s[NWM_SCODE] = W(0x30); p->e[NWM_SCODE] = W(0x2C);
            p->s[NWM_SDATA] = W(0x3C); p->e[NWM_SDATA] = W(0x38);
        } else if (W(0x0C) == DB_DST && W(0x18) == CODE_DST) {
            /* "Older Large Pool", main.dst at +18h; type1/4 in its tail
               (+38h..+44h, 3EDh at +48h) or in the "Older Small Pool" */
            p->layout = NWM_OLDER;
            p->s[NWM_T5] = W(0x08); p->e[NWM_T5] = W(0x04);
            p->s[NWM_DB] = W(0x14); p->e[NWM_DB] = W(0x10);
            p->s[NWM_SCODE] = W(0x20); p->e[NWM_SCODE] = W(0x1C);
            p->s[NWM_SDATA] = W(0x28); p->e[NWM_SDATA] = W(0x24);
            if (W(0x48) == POOL_3ED) {
                p->s[NWM_T1] = W(0x3C); p->e[NWM_T1] = W(0x38);
                p->s[NWM_T4] = W(0x44); p->e[NWM_T4] = W(0x40);
            } else {
                for (u32 j = 0; j + 0x20 <= len; j += 4) {
                    if (rd32(c, len, j) == MAIN_DST && rd32(c, len, j + 0x1C) == POOL_3ED) {
                        p->s[NWM_T1] = rd32(c, len, j + 0x10); p->e[NWM_T1] = rd32(c, len, j + 0x0C);
                        p->s[NWM_T4] = rd32(c, len, j + 0x18); p->e[NWM_T4] = rd32(c, len, j + 0x14);
                        break;
                    }
                }
            }
        } else {
            continue;
        }
#undef W
        *at = i;
        return 1;
    }
    return 0;
}

static const u32 fixed_size[NWM_NBLK] = { [NWM_SDATA] = 0x38, [NWM_SCODE] = 0x316, [NWM_DB] = 0x1E8 };

int nwm_extract(u8 *code, u32 len, u32 cap, u32 main_type, u8 *out, u32 outcap, u32 *outlen,
                nwm_info_t *info)
{
    static const u32 bases[2] = { 0x00100000u, 0x00010000u };  /* 3DS .text; GBATEK writes 10000h */
    int saw_pool = 0, saw_sizes = 0;
    u32 mt = main_type == 1 ? NWM_T1 : main_type == 5 ? NWM_T5 : NWM_T4;
    memset(info, 0, sizeof(*info));
    info->in_size = len;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {                        /* not found as is: maybe still BLZ-compressed */
            if (!blz_ok(code, len)) break;
            int r = blz_decompress(code, len, cap, &len);
            if (r) return r;
            info->blz = 1;
        }
        info->code_size = len;
        pool_t p;
        u32 at = 0;
        for (u32 from = 0; find_pool(code, len, from, &p, &at); from = at + 4) {
            saw_pool = 1;
            for (int bi = 0; bi < 2; bi++) {
                u32 base = bases[bi], ok = 1;
                for (int k = NWM_SDATA; k <= NWM_DB; k++)
                    if (!(p.s[k] >= base && p.s[k] < p.e[k] && p.e[k] - base <= len) ||
                        p.e[k] - p.s[k] != fixed_size[k])
                        ok = 0;
                if (!ok) continue;
                saw_sizes = 1;
                for (int k = NWM_T1; k <= NWM_T5; k++)
                    if (p.s[k] >= base && p.s[k] < p.e[k] && p.e[k] - base <= len)
                        info->main_size[k - NWM_T1] = p.e[k] - p.s[k];
                info->layout = p.layout;
                info->text_base = base;
                info->pool_off = at;
                info->main_type = mt == NWM_T1 ? 1 : mt == NWM_T5 ? 5 : 4;
                info->db_end_is_stub = p.e[NWM_DB] == p.s[NWM_SCODE];
                if (!info->main_size[mt - NWM_T1]) return NWM_E_NOMAIN;
                /* container: 16-byte header, 8 part slots, the 4 blocks (nwmfw.py pack) */
                static const u8 order[4] = { NWM_SDATA, NWM_SCODE, NWM_DB, 0 };
                static const u32 dst[4] = { MAIN_DST, CODE_DST, DB_DST, MAIN_DST };
                u32 hdr = 16 + 8 * 16, pos = hdr;
                for (int i = 0; i < 4; i++) {
                    u32 k = order[i] ? order[i] : mt;
                    pos += ((p.e[k] - p.s[k]) + 3) & ~3u;
                }
                if (pos > outcap) return NWM_E_TOOBIG;
                memset(out, 0, pos);
                wr32(out, I3WF_MAGIC); wr32(out + 4, 1); wr32(out + 8, 4); wr32(out + 12, CHIP_AR6014);
                pos = hdr;
                for (int i = 0; i < 4; i++) {
                    u32 k = order[i] ? order[i] : mt, n = p.e[k] - p.s[k];
                    u8 *e = out + 16 + 16 * i;
                    wr32(e, (u32)i + 1); wr32(e + 4, dst[i]); wr32(e + 8, pos); wr32(e + 12, n);
                    memcpy(out + pos, code + (p.s[k] - base), n);
                    info->blk_size[i] = n;
                    info->blk_crc[i] = nwm_crc32(out + pos, n);
                    pos += (n + 3) & ~3u;
                }
                *outlen = pos;
                return 0;
            }
        }
    }
    return saw_sizes ? NWM_E_NOMAIN : saw_pool ? NWM_E_SIZES : NWM_E_NOPOOL;
}

const char *nwm_strerror(int e)
{
    switch (e) {
    case NWM_E_NOPOOL: return "no NWM firmware pool (is this NWM's .code, title 00040130-00002d02?)";
    case NWM_E_SIZES:  return "pool found but the stub/database sizes differ from GBATEK (38h/316h/1E8h)";
    case NWM_E_NOMAIN: return "the requested main firmware type is not referenced by this NWM";
    case NWM_E_BLZ:    return "BLZ decompression failed (truncated or corrupt .code)";
    case NWM_E_TOOBIG: return "does not fit the buffer";
    default:           return "?";
    }
}
