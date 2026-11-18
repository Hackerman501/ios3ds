/*
 * ios3ds QEMU board: minimal models of the 3DS CSND sound block (0x10103000)
 * and the MIC FIFO (0x10162000, at the end of this file).
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

/* ======================================================================== */
/*
 * MIC (0x10162000): the microphone FIFO behind the CODEC's ADC (I2S2).
 * Layout as GBATEK documents the DSi's (the 3DS block is the same design):
 *   0x000 MIC_CNT (u16): 2-3 rate divider (I2S2 rate / (1 + n)), 8 FIFO empty,
 *         9 half full (>= 8 words), 10 full (16 words), 11 overrun, 12 clear
 *         (write 1), 13-14 IRQ mode (not modelled), 15 enable
 *   0x004 MIC_DATA (u32): two signed 16-bit samples, older one in the low half
 * Samples are produced on the virtual clock at 47605 / (1 + n) Hz (I2S2 as
 * boot/arm9/audio.c programs it) from IOS3DS_MIC:
 *   ramp           a 16-bit counter (sample k = k & 0xFFFF): checks the whole
 *                  path for dropped, repeated-out-of-order or altered samples
 *   sine:HZ:AMP    a sine, e.g. sine:440:8000
 *   /path/x.wav    16-bit PCM (first channel), looped, one sample per tick
 *   unset          silence (the FIFO still runs)
 * IOS3DS_MIC_FIFO=n (samples) enlarges the FIFO, to tell overruns caused by a
 * slow emulated ARM9 apart from anything the rest of the path does.
 */
#define MIC_FIFO_DEFAULT 32     /* 16 words, as GBATEK documents; IOS3DS_MIC_FIFO=n overrides (test aid) */
#define MIC_FIFO_SAMPLES M.fifo
#define MIC_I2S2_HZ      47605

static struct {
    MemoryRegion io;
    uint16_t cnt;
    int64_t t0;             /* virtual time of sample 0 since enable */
    uint64_t made, taken;   /* samples produced / read since enable */
    int mode;               /* 0 silence, 1 ramp, 2 sine, 3 wav */
    double hz, amp;
    int16_t *wav;
    uint32_t wav_n;
    uint64_t total_taken, overruns;
    int64_t stat_ns;
    uint32_t fifo;
} M;

static uint32_t mic_rate(void)
{
    return MIC_I2S2_HZ / (1 + ((M.cnt >> 2) & 3));
}

static int16_t mic_sample(uint64_t k)
{
    switch (M.mode) {
    case 1: return (int16_t)(uint16_t)k;
    case 2: return (int16_t)lrint(M.amp * sin(2 * M_PI * M.hz * (double)k / mic_rate()));
    case 3: return M.wav_n ? M.wav[k % M.wav_n] : 0;
    default: return 0;
    }
}

/* bring the FIFO up to now: samples made but not read beyond the FIFO are lost */
static void mic_advance(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t due;
    if (!(M.cnt & 0x8000)) {
        return;
    }
    due = muldiv64(now - M.t0, mic_rate(), NANOSECONDS_PER_SECOND);
    if (due > M.made) {
        M.made = due;
    }
    if (M.made - M.taken > MIC_FIFO_SAMPLES) {
        M.taken = M.made - MIC_FIFO_SAMPLES;    /* oldest samples dropped */
        M.cnt |= 1u << 11;
        M.overruns++;
    }
}

static uint64_t mic_read(void *opaque, hwaddr addr, unsigned size)
{
    mic_advance();
    if (addr < 4) {
        uint32_t n = (uint32_t)(M.made - M.taken), v = M.cnt & ~0x0700u;
        if (!(M.cnt & 0x8000) || n < 2) v |= 1u << 8;
        if (n >= MIC_FIFO_SAMPLES / 2) v |= 1u << 9;
        if (n >= MIC_FIFO_SAMPLES) v |= 1u << 10;
        return extract32(v, (addr & 3) * 8, size * 8);
    }
    if (addr >= 4 && addr < 8) {
        uint32_t v = 0;
        if ((M.cnt & 0x8000) && M.made - M.taken >= 2) {
            v = (uint16_t)mic_sample(M.taken) | (uint32_t)(uint16_t)mic_sample(M.taken + 1) << 16;
            M.taken += 2;
            M.total_taken += 2;
        }
        if (M.total_taken && M.total_taken % (5u * MIC_I2S2_HZ / 4) < 2) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (now - M.stat_ns > NANOSECONDS_PER_SECOND) {
                fprintf(stderr, "[mic] %" PRIu64 " samples read at %u Hz, %" PRIu64 " overruns\n",
                        M.total_taken, mic_rate(), M.overruns);
                M.stat_ns = now;
            }
        }
        return v;
    }
    return 0;
}

static void mic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint16_t old = M.cnt, v;
    if (addr >= 4) {
        return;
    }
    v = (uint16_t)(size >= 2 ? val : (addr & 1 ? (old & 0xFF) | (val << 8) : (old & 0xFF00) | val));
    mic_advance();
    M.cnt = (v & ~0x1F00u) | (old & (1u << 11) & v);   /* overrun stays until written 0 */
    if ((v & 0x8000) && (!(old & 0x8000) || (v & 0x1000))) {     /* enable or clear: empty FIFO */
        M.t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        M.made = M.taken = 0;
        M.cnt &= ~(1u << 11);
        if (!(old & 0x8000)) {
            fprintf(stderr, "[mic] enabled: %u Hz, source %s, FIFO %u samples\n", mic_rate(),
                    (const char *[]){ "silence", "ramp", "sine", "wav" }[M.mode], M.fifo);
        }
    }
}

static const MemoryRegionOps mic_ops = {
    .read = mic_read,
    .write = mic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mic_load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12], ck[8];
    uint16_t ch = 1, bits = 16;
    if (!f) {
        fprintf(stderr, "[mic] cannot read %s: silence\n", path);
        return;
    }
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        while (fread(ck, 1, 8, f) == 8) {
            uint32_t len = ldl_le_p(ck + 4);
            if (!memcmp(ck, "fmt ", 4) && len >= 16) {
                uint8_t fmt[16];
                if (fread(fmt, 1, 16, f) != 16) break;
                ch = lduw_le_p(fmt + 2);
                bits = lduw_le_p(fmt + 14);
                fseek(f, len - 16, SEEK_CUR);
            } else if (!memcmp(ck, "data", 4) && bits == 16 && ch) {
                uint32_t frames = len / (2 * ch);
                int16_t *all = g_new(int16_t, (size_t)frames * ch);
                frames = fread(all, 2 * ch, frames, f);
                M.wav = g_new(int16_t, frames ? frames : 1);
                for (uint32_t i = 0; i < frames; i++) {
                    M.wav[i] = (int16_t)lduw_le_p(&all[(size_t)i * ch]);
                }
                g_free(all);
                M.wav_n = frames;
                M.mode = 3;
                break;
            } else {
                fseek(f, len + (len & 1), SEEK_CUR);
            }
        }
    }
    fclose(f);
    if (M.mode != 3) {
        fprintf(stderr, "[mic] %s is not 16-bit PCM WAV: silence\n", path);
    }
}

void ctr_mic_init(MemoryRegion *parent, hwaddr offset)
{
    const char *src = getenv("IOS3DS_MIC"), *fifo = getenv("IOS3DS_MIC_FIFO");
    M.fifo = fifo && atoi(fifo) >= 2 ? (uint32_t)atoi(fifo) & ~1u : MIC_FIFO_DEFAULT;
    if (src && !strcmp(src, "ramp")) {
        M.mode = 1;
    } else if (src && !strncmp(src, "sine", 4)) {
        M.mode = 2;
        M.hz = 440;
        M.amp = 8000;
        sscanf(src, "sine:%lf:%lf", &M.hz, &M.amp);
    } else if (src && *src) {
        mic_load_wav(src);
    }
    memory_region_init_io(&M.io, NULL, &mic_ops, NULL, "ctr.mic", 0x100);
    memory_region_add_subregion(parent, offset, &M.io);
}
