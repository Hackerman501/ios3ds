/*
 * ios3ds-pica: command list encoder.
 *
 * One command:
 *     word 0   first parameter
 *     word 1   header:  bits 0-15  register index
 *                       bits 16-19 byte-enable mask for the write
 *                       bits 20-27 number of extra parameters
 *                       bit 31     1 = extra parameters go to reg+1, reg+2, ...
 *                                  0 = they all go to the same register
 *     word 2.. extra parameters, then a zero word if needed to end on an
 *              8-byte boundary.
 * The GPU stops at a write to FINALIZE that matches IRQ_CMP; the list handed
 * to CMDBUF_SIZE0 must be a multiple of 16 bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "pica.h"

#define HDR(reg, mask, extra, seq) \
    (((reg) & 0xFFFFu) | (((mask) & 0xFu) << 16) | (((extra) & 0xFFu) << 20) | ((seq) ? 0x80000000u : 0u))

void pica_cmd_init(pica_cmd_t *c, uint32_t *buf, uint32_t cap_words)
{
    c->buf = buf;
    c->cap = cap_words;
    c->len = 0;
    c->overflow = 0;
}

/* One group of 1..256 parameters. */
static void put(pica_cmd_t *c, uint32_t reg, uint32_t mask, const uint32_t *vals, uint32_t n, int seq)
{
    uint32_t words = 1 + n + ((n & 1) ? 0 : 1);     /* header + params, padded to even */

    if (c->overflow || c->len + words > c->cap) {
        c->overflow = 1;
        return;
    }
    uint32_t *p = c->buf + c->len;
    p[0] = vals[0];
    p[1] = HDR(reg, mask, n - 1, seq);
    if (n > 1)
        memcpy(p + 2, vals + 1, (n - 1) * 4);
    if (!(n & 1))
        p[words - 1] = 0;
    c->len += words;
}

static void put_many(pica_cmd_t *c, uint32_t reg, const uint32_t *vals, uint32_t n, int seq)
{
    while (n) {
        uint32_t k = n > 256 ? 256 : n;
        put(c, reg, 0xF, vals, k, seq);
        vals += k;
        n -= k;
        if (seq)
            reg += k;
    }
}

void pica_cmd_write(pica_cmd_t *c, uint32_t reg, uint32_t value)
{
    put(c, reg, 0xF, &value, 1, 0);
}

void pica_cmd_write_masked(pica_cmd_t *c, uint32_t reg, uint32_t bytemask, uint32_t value)
{
    put(c, reg, bytemask, &value, 1, 0);
}

void pica_cmd_write_seq(pica_cmd_t *c, uint32_t reg, const uint32_t *vals, uint32_t n)
{
    put_many(c, reg, vals, n, 1);
}

void pica_cmd_write_burst(pica_cmd_t *c, uint32_t reg, const uint32_t *vals, uint32_t n)
{
    put_many(c, reg, vals, n, 0);
}

uint32_t pica_cmd_finish(pica_cmd_t *c)
{
    pica_cmd_write(c, P3D_FINALIZE, PICA_FINALIZE_MAGIC);
    if (c->len & 3)                     /* commands are 8 bytes; make it 16 */
        pica_cmd_write(c, P3D_FINALIZE, PICA_FINALIZE_MAGIC);
    return c->overflow ? 0 : c->len * 4;
}

/* The GPU's floats: sign, 7-bit exponent (bias 63), then 8 / 16 / 23 mantissa
   bits (f16: 5-bit exponent, bias 15, 10 mantissa bits). Truncating. */
static uint32_t to_float(float f, int exp_bits, int man_bits)
{
    uint32_t i;
    memcpy(&i, &f, 4);
    uint32_t sign = i >> 31;
    int32_t  exp = (int32_t)((i >> 23) & 0xFF);
    uint32_t man = (i & 0x7FFFFF) >> (23 - man_bits);
    int32_t  emax = (1 << exp_bits) - 1;

    exp = exp - 127 + (emax >> 1);
    if (exp < 0)                        /* underflow (including zero): signed zero */
        return sign << (exp_bits + man_bits);
    if (exp > emax)                     /* overflow: infinity */
        return sign << (exp_bits + man_bits) | (uint32_t)emax << man_bits;
    return sign << (exp_bits + man_bits) | (uint32_t)exp << man_bits | man;
}

uint32_t pica_f16(float f) { return to_float(f, 5, 10); }
uint32_t pica_f24(float f) { return to_float(f, 7, 16); }
uint32_t pica_f31(float f) { return to_float(f, 7, 23); }
