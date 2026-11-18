/*
 * ios3ds QEMU board: a minimal model of the 3DS CSND sound block (0x10103000).
 *
 * Enough for the kernel shim's audio path (kernel/shim/audio.c) to be checked
 * end to end in the emulator: channels play PCM16/PCM8 from FCRAM at their
 * own rate against QEMU's virtual clock, the start-address register reads back
 * the play position (the shim re-aligns its write pointer to it), one-shot
 * channels stop at the end, and everything that is "played" is mixed into a
 * stereo sink that keeps statistics and, if asked, writes a WAV file:
 *
 *   IOS3DS_CSND_WAV=/path/out.wav   record the mix (16-bit stereo, first
 *                                   playing channel's rate)
 *
 * Not modelled: ADPCM, capture units, the resample filter, I2S1/CODEC.
 * Register layout from libn3ds include/arm11/drivers/csnd.h:
 *   0x000 MASTER_VOL (u16, 0..0x8000), 0x002 CNT (u16, bit 15 enable, bit 0 mute)
 *   0x400 + 0x20 * n: CNT (u16: 15 start, 14 playing, 12-13 format 0 PCM8
 *   1 PCM16 2 ADPCM, 10-11 repeat 0 manual 1 loop 2 one-shot), SR (u16,
 *   0x10000 - 67027964 / Fs), VOL (u32: right low, left high), CAPVOL,
 *   ST_ADDR (reads the play position), SIZE (bytes), LP_ADDR.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "qemu/log.h"
#include "system/memory.h"
#include "ctr_csnd.h"

#define CSND_CLOCK      67027964u
#define NCH             32
#define TICK_NS         (5 * SCALE_MS)
#define STAT_NS         (5 * NANOSECONDS_PER_SECOND)

typedef struct {
    uint16_t cnt, sr;
    uint32_t vol, capvol, st, size, lp;
    bool playing;
    uint64_t pos;           /* play position in bytes from st, 32.32 fixed point */
} CsndCh;

static struct {
    MemoryRegion io;
    MemoryRegion *fcram;
    hwaddr fcram_base;
    uint8_t *ram;
    uint64_t ram_size;
    uint16_t master_vol, cnt;
    CsndCh ch[NCH];
    QEMUTimer *tick;
    int64_t last_ns, stat_ns;
    uint64_t frac;                      /* output frames, 32.32 */
    uint32_t out_rate;
    /* statistics of the mix */
    uint64_t frames, nonzero, frames_window;
    uint32_t peak_l, peak_r, peak_window;
    double sq_window;
    /* WAV sink */
    FILE *wav;
    uint32_t wav_frames, wav_rate;
} C;

static uint32_t ch_rate(const CsndCh *c)
{
    uint32_t div = 0x10000u - c->sr;
    return div ? CSND_CLOCK / div : 0;
}

static int ch_bps(const CsndCh *c)              /* bytes per sample, 0 = unsupported */
{
    switch ((c->cnt >> 12) & 3) {
    case 0: return 1;
    case 1: return 2;
    default: return 0;
    }
}

static const uint8_t *pa_ptr(uint32_t pa, uint32_t len)
{
    if (pa < C.fcram_base || pa + (uint64_t)len > C.fcram_base + C.ram_size) {
        return NULL;
    }
    return C.ram + (pa - C.fcram_base);
}

static void wav_header(void)
{
    uint8_t h[44];
    uint32_t data = C.wav_frames * 4;
    memcpy(h, "RIFF", 4); stl_le_p(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    stl_le_p(h + 16, 16); stw_le_p(h + 20, 1); stw_le_p(h + 22, 2);
    stl_le_p(h + 24, C.wav_rate); stl_le_p(h + 28, C.wav_rate * 4);
    stw_le_p(h + 32, 4); stw_le_p(h + 34, 16);
    memcpy(h + 36, "data", 4); stl_le_p(h + 40, data);
    if (fseek(C.wav, 0, SEEK_SET) == 0 && fwrite(h, 1, 44, C.wav) == 44) {
        fseek(C.wav, 0, SEEK_END);
    }
}

static void wav_open(uint32_t rate)
{
    const char *path = getenv("IOS3DS_CSND_WAV");
    if (C.wav || !path || !*path || !rate) {
        return;
    }
    C.wav = fopen(path, "wb");
    if (!C.wav) {
        fprintf(stderr, "[csnd] cannot write %s\n", path);
        return;
    }
    C.wav_rate = rate;
    C.wav_frames = 0;
    wav_header();
    fprintf(stderr, "[csnd] recording the mix to %s (%u Hz)\n", path, rate);
}

static bool any_playing(void)
{
    for (int i = 0; i < NCH; i++) {
        if (C.ch[i].playing) {
            return true;
        }
    }
    return false;
}

static uint32_t first_rate(void)
{
    for (int i = 0; i < NCH; i++) {
        if (C.ch[i].playing && ch_rate(&C.ch[i])) {
            return ch_rate(&C.ch[i]);
        }
    }
    return 0;
}

/* one sample of channel c at its current position, then advance by step (32.32 bytes) */
static int32_t ch_sample(CsndCh *c, uint64_t step)
{
    int bps = ch_bps(c);
    uint32_t off = (uint32_t)(c->pos >> 32) & ~(uint32_t)(bps - 1);
    const uint8_t *p;
    int32_t s = 0;

    if (!bps || off >= c->size) {
        c->playing = false;
        return 0;
    }
    p = pa_ptr(c->st + off, bps);
    if (p) {
        s = bps == 2 ? (int16_t)lduw_le_p(p) : (int8_t)p[0] * 256;
    }
    c->pos += step;
    if ((c->pos >> 32) >= c->size) {
        switch ((c->cnt >> 10) & 3) {
        case 1:                                 /* loop: restart at LP_ADDR */
            c->pos = ((uint64_t)(c->lp - c->st) << 32) + (c->pos - ((uint64_t)c->size << 32));
            if ((c->pos >> 32) >= c->size) {
                c->pos = (uint64_t)(c->lp - c->st) << 32;
            }
            break;
        default:                                /* one-shot / manual: the channel ends */
            c->playing = false;
            c->cnt &= ~0xC000;
            break;
        }
    }
    return s;
}

static void csnd_run(int64_t now)
{
    uint64_t want;
    uint32_t n;

    if (!C.out_rate) {
        C.out_rate = first_rate();
        wav_open(C.out_rate);
    }
    if (!C.out_rate || now <= C.last_ns) {
        C.last_ns = now;
        return;
    }
    want = C.frac + (uint64_t)(((unsigned __int128)(now - C.last_ns) * C.out_rate << 32)
                               / NANOSECONDS_PER_SECOND);
    n = (uint32_t)(want >> 32);
    C.frac = want & 0xFFFFFFFFu;
    C.last_ns = now;
    if (n > C.out_rate) {
        n = C.out_rate;                         /* at most one second per step */
    }
    for (uint32_t f = 0; f < n; f++) {
        int64_t l = 0, r = 0;
        for (int i = 0; i < NCH; i++) {
            CsndCh *c = &C.ch[i];
            uint32_t rate;
            int32_t s;
            if (!c->playing) {
                continue;
            }
            rate = ch_rate(c);
            s = ch_sample(c, ((uint64_t)rate * ch_bps(c) << 32) / C.out_rate);
            l += (int64_t)s * (c->vol >> 16) >> 15;
            r += (int64_t)s * (c->vol & 0xFFFF) >> 15;
        }
        l = l * C.master_vol >> 15;
        r = r * C.master_vol >> 15;
        if (!(C.cnt & 0x8000) || (C.cnt & 1)) {
            l = r = 0;
        }
        l = MAX(-32768, MIN(32767, l));
        r = MAX(-32768, MIN(32767, r));
        C.frames++;
        C.frames_window++;
        if (l || r) {
            C.nonzero++;
        }
        C.peak_l = MAX(C.peak_l, (uint32_t)ABS(l));
        C.peak_r = MAX(C.peak_r, (uint32_t)ABS(r));
        C.peak_window = MAX(C.peak_window, (uint32_t)MAX(ABS(l), ABS(r)));
        C.sq_window += (double)l * l + (double)r * r;
        if (C.wav) {
            uint8_t fr[4];
            stw_le_p(fr, (uint16_t)l);
            stw_le_p(fr + 2, (uint16_t)r);
            if (fwrite(fr, 1, 4, C.wav) == 4) {
                C.wav_frames++;
            }
        }
    }
    if (C.wav && n) {
        wav_header();
        fflush(C.wav);
    }
    if (now - C.stat_ns >= STAT_NS) {
        if (C.peak_window) {
            fprintf(stderr, "[csnd] %u Hz: %" PRIu64 " frames, %" PRIu64 " non-silent, "
                    "last %.1f s peak %u rms %.0f\n", C.out_rate, C.frames, C.nonzero,
                    (double)(now - C.stat_ns) / NANOSECONDS_PER_SECOND, C.peak_window,
                    C.frames_window ? sqrt(C.sq_window / (2.0 * C.frames_window)) : 0.0);
        }
        C.stat_ns = now;
        C.frames_window = 0;
        C.peak_window = 0;
        C.sq_window = 0;
    }
}

static void csnd_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    csnd_run(now);
    if (any_playing()) {
        timer_mod(C.tick, now + TICK_NS);
    } else {
        C.out_rate = 0;                         /* next start picks its rate again */
    }
}

static void ch_cnt_write(int i, uint16_t v)
{
    CsndCh *c = &C.ch[i];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool was = c->playing, start = !(c->cnt & 0x8000) && (v & 0x8000);

    csnd_run(now);                              /* everything up to now at the old settings */
    c->cnt = v;
    if (start) {
        c->pos = 0;
    }
    c->playing = (v & 0x8000) && (v & 0x4000) && ch_bps(c) && c->size;
    if (c->playing && !was) {
        fprintf(stderr, "[csnd] ch%d play pa=%08x size=%u loop=%08x %u Hz %s vol=%08x\n", i,
                c->st, c->size, c->lp, ch_rate(c), ch_bps(c) == 2 ? "pcm16" : "pcm8", c->vol);
        if (!timer_pending(C.tick)) {
            C.last_ns = now;
            C.frac = 0;
            C.stat_ns = now;
            timer_mod(C.tick, now + TICK_NS);
        }
    } else if (!c->playing && was) {
        fprintf(stderr, "[csnd] ch%d stop (mix so far: %" PRIu64 " frames, %" PRIu64
                " non-silent, peak L %u R %u)\n", i, C.frames, C.nonzero, C.peak_l, C.peak_r);
    }
}

static uint64_t csnd_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t v = 0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (any_playing()) {
        csnd_run(now);
    }
    if (addr < 4) {
        v = C.master_vol | (uint32_t)C.cnt << 16;
    } else if (addr >= 0x400 && addr < 0x400 + NCH * 0x20) {
        CsndCh *c = &C.ch[(addr - 0x400) >> 5];
        switch ((addr & 0x1F) & ~3) {
        case 0x00: v = c->cnt | (uint32_t)c->sr << 16; break;
        case 0x04: v = c->vol; break;
        case 0x08: v = c->capvol; break;
        case 0x0C: v = c->st + (c->playing ? (uint32_t)(c->pos >> 32) : 0); break;
        case 0x10: v = c->size; break;
        case 0x14: v = c->lp; break;
        }
    }
    return extract32(v, (addr & 3) * 8, size * 8);
}

static void csnd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t sh = (addr & 3) * 8, mask = size == 4 ? ~0u : ((1u << (size * 8)) - 1) << sh;
    uint32_t v = (uint32_t)(val << sh);

#define MERGE(f) ((f) = ((f) & ~mask) | (v & mask))
    if (addr < 4) {
        uint32_t w = C.master_vol | (uint32_t)C.cnt << 16;
        MERGE(w);
        csnd_run(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        C.master_vol = w & 0xFFFF;
        C.cnt = w >> 16;
    } else if (addr >= 0x400 && addr < 0x400 + NCH * 0x20) {
        int i = (addr - 0x400) >> 5;
        CsndCh *c = &C.ch[i];
        switch ((addr & 0x1F) & ~3) {
        case 0x00: {
            uint32_t w = c->cnt | (uint32_t)c->sr << 16;
            MERGE(w);
            c->sr = w >> 16;
            if (mask & 0xFFFF) {
                ch_cnt_write(i, w & 0xFFFF);
            }
            break;
        }
        case 0x04: csnd_run(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)); MERGE(c->vol); break;
        case 0x08: MERGE(c->capvol); break;
        case 0x0C: MERGE(c->st); break;
        case 0x10: MERGE(c->size); break;
        case 0x14: MERGE(c->lp); break;
        }
    }
#undef MERGE
}

static const MemoryRegionOps csnd_ops = {
    .read = csnd_read,
    .write = csnd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void ctr_csnd_init(MemoryRegion *parent, hwaddr offset, MemoryRegion *fcram, hwaddr fcram_base)
{
    C.fcram = fcram;
    C.fcram_base = fcram_base;
    C.ram = memory_region_get_ram_ptr(fcram);
    C.ram_size = memory_region_size(fcram);
    C.tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, csnd_tick, NULL);
    memory_region_init_io(&C.io, NULL, &csnd_ops, NULL, "ctr.csnd", 0x1000);
    memory_region_add_subregion(parent, offset, &C.io);
}
