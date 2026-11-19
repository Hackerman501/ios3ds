/*
 * Host test for the shim's audio emulation (kernel/shim/audio.c,
 * built with -DAUD_HOST_TEST). The "machine" here is a block of fake FCRAM, a
 * PL080 register page and a CSND register page; the test programs the PL080
 * the way AppleARMPL080DMAC does (docs/audio.md) and checks what the emulator
 * makes of it. Run by tests/test_audio.py (make test).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ios3ds.h"
#include "audio.h"

/* ---- the machine ---------------------------------------------------------- */
#define MEM_PA   0x20800000u
#define MEM_SIZE 0x00200000u            /* covers AUD_PCM_PA and the test's DMA buffers */
static u8 mem[MEM_SIZE];
u32 aud_host_dmac[0x400];
int aud_host_csnd_present = 1;
u8 aud_host_csnd[0x1000];
static char logbuf[8192];
static u32 loglen;

void *aud_host_p2v(u32 pa, u32 len)
{
    if (pa < MEM_PA || pa + len < pa || pa + len > MEM_PA + MEM_SIZE) return NULL;
    return mem + (pa - MEM_PA);
}
void aud_host_log(int c) { if (loglen < sizeof(logbuf) - 1) logbuf[loglen++] = (char)c; }

/* the MIC block: a FIFO the test fills (hmic_push) */
static u32 hmic_cnt, hmic_q[4096], hmic_head, hmic_tail, hmic_next;
u32 aud_host_mic_cnt_r(void) { return hmic_cnt | (hmic_head == hmic_tail ? MIC_CNT_EMPTY : 0); }
void aud_host_mic_cnt_w(u32 v)
{
    hmic_cnt = v & ~(MIC_CNT_CLEAR | MIC_CNT_EMPTY);
    if (v & MIC_CNT_CLEAR) hmic_head = hmic_tail;
}
u32 aud_host_mic_data_r(void)
{
    if (hmic_head == hmic_tail) return 0;
    return hmic_q[hmic_tail++ & 4095];
}
static void hmic_push(u32 samples)              /* a ramp of 6 per sample, two per FIFO word */
{
    for (u32 i = 0; i < samples; i += 2, hmic_next += 2)
        hmic_q[hmic_head++ & 4095] = (u16)(int16_t)((int)hmic_next * 6 - 3000) |
                                     (u32)(u16)(int16_t)((int)(hmic_next + 1) * 6 - 3000) << 16;
}

extern volatile aud_blk_t shim_audio;
void aud_service(u32 now);
int shim_iic_transfer(void *nub, u32 op, u32 sublen, const u8 *sub, u32 len, u8 *buf);
u32 aud_test_inject(void);
u32 aud_test_next(u32 *valid);
u32 aud_test_wr(void);
void aud_test_reset(void);

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- PL080 as the Apple driver programs it -------------------------------- */
#define CH(n, reg)  aud_host_dmac[(0x100 + (n) * 0x20) / 4 + (reg)]
enum { SRC, DST, LLI, CTL, CFG };
#define I2S_TX_FIFO 0x3CA00010u
#define I2S_RX_FIFO 0x3CA00038u
#define TX_CTL      0x03249000u         /* device tree dma-channels: 16-bit, burst 4 */
#define TX_CFG      0x00000A80u         /* dest peripheral 10, memory -> peripheral */
#define RX_CFG      0x00001056u         /* src peripheral 11, peripheral -> memory */
#define SI          (1u << 26)
#define DI          (1u << 27)
#define IBIT        (1u << 31)
#define LLI_PA      0x20810000u         /* the driver's 4 KiB LLI page (256 items) */
#define BUF_PA      0x20820000u         /* iOS's 64 KiB sample ring (16384 stereo frames) */
#define RING_BYTES  0x10000u
#define MAX_ITEM    0xE00u              /* words per LLI (AppleARMPL080DMAC 0x807d01c4) */

static u32 *lli(u32 idx) { return (u32 *)aud_host_p2v(LLI_PA + idx * 16, 16); }

/* queue one "whole ring" command at LLI index `first`; returns the next free index */
static u32 queue_ring(u32 first, u32 pa, u32 bytes, int tx)
{
    u32 idx = first, words = bytes / 2;
    while (words) {
        u32 n = words > MAX_ITEM ? MAX_ITEM : words, *q = lli(idx);
        words -= n;
        q[0] = tx ? pa : I2S_RX_FIFO;
        q[1] = tx ? I2S_TX_FIFO : pa;
        q[2] = 0;
        q[3] = TX_CTL | (tx ? SI : DI) | n | (words ? 0 : IBIT);
        if (idx != first) lli(idx - 1)[2] = LLI_PA + idx * 16;
        pa += n * 2;
        idx++;
    }
    return idx;
}

static void start_chan(u32 ch, u32 first, u32 cfg)
{
    u32 *q = lli(first);
    CH(ch, LLI) = q[2];
    CH(ch, SRC) = q[0];
    CH(ch, DST) = q[1];
    CH(ch, CTL) = q[3];
    CH(ch, CFG) = cfg | 0x8001;         /* E + terminal-count interrupt enable */
}

static void reset(void)
{
    memset(mem, 0, sizeof(mem));
    memset(aud_host_dmac, 0, sizeof(aud_host_dmac));
    memset(aud_host_csnd, 0, sizeof(aud_host_csnd));
    loglen = 0;
    aud_test_reset();
    shim_audio.cfg.dmac_pa = 0x1FFBC000;
}

static int16_t *pcm(void) { return (int16_t *)aud_host_p2v(AUD_PCM_PA, AUD_RING_FRAMES * 4); }
static u32 csnd16(u32 off) { return aud_host_csnd[off] | (u32)aud_host_csnd[off + 1] << 8; }
static u32 csnd32(u32 off) { return csnd16(off) | csnd16(off + 2) << 16; }

/* 1 tick = 1/1047311.9 s; 44097.34 Hz stereo = 8/95 words per tick */
static u32 words_at(u32 ticks) { return (u32)(((unsigned long long)ticks * 128) / AUD_RATE_DIV); }

static void test_tx_stream(void)
{
    reset();
    int16_t *src = (int16_t *)aud_host_p2v(BUF_PA, RING_BYTES);
    for (u32 i = 0; i < RING_BYTES / 2; i++) src[i] = (int16_t)((i & 1) ? -(int)(i / 2) - 1 : (int)(i / 2) + 1);
    u32 end = queue_ring(0, BUF_PA, RING_BYTES, 1);
    CHECK(end == 10, "64 KiB ring = 10 LLIs, got %u", end);
    start_chan(0, 0, TX_CFG);

    u32 t = 1000;
    aud_service(t);                     /* first sight: stream start, nothing consumed yet */
    CHECK(shim_audio.stat.starts == 1, "starts %u", shim_audio.stat.starts);
    CHECK(shim_audio.stat.tx_words == 0, "consumed before any time passed");
    CHECK(strstr(logbuf, "tx start ch0") != NULL, "start not logged: %s", logbuf);
    /* CSND programmed: two looping PCM16 channels over the two rings */
    CHECK(csnd16(CSND_CNT) == (CSND_CNT_EN | CSND_CNT_RS_FILTER), "CSND_CNT %x", csnd16(CSND_CNT));
    CHECK(csnd16(CSND_CH(0) + CSND_CH_SR) == 0x10000 - 1520, "sample rate reg %x", csnd16(CSND_CH(0) + CSND_CH_SR));
    CHECK(csnd32(CSND_CH(0) + CSND_CH_ADDR) == AUD_PCM_PA, "L addr");
    CHECK(csnd32(CSND_CH(1) + CSND_CH_ADDR) == AUD_PCM_PA + AUD_RING_FRAMES * 2, "R addr");
    CHECK(csnd32(CSND_CH(0) + CSND_CH_SIZE) == AUD_RING_FRAMES * 2, "size");
    CHECK(csnd32(CSND_CH(0) + CSND_CH_VOL) == 0x80000000u && csnd32(CSND_CH(1) + CSND_CH_VOL) == 0x8000u, "pan");
    u32 want = CSND_CH_START | CSND_CH_PLAYING | CSND_CH_FMT_PCM16 | CSND_CH_RPT_LOOP | CSND_CH_LERP;
    CHECK(csnd16(CSND_CH(0) + CSND_CH_CNT) == want && csnd16(CSND_CH(1) + CSND_CH_CNT) == want, "ch cnt %x", csnd16(CSND_CH(0) + CSND_CH_CNT));

    /* run 100 ms in uneven steps: consumption follows the clock exactly */
    u32 t0 = t, steps[] = { 17455, 3, 17000, 40000, 1, 30272 };
    for (u32 i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        t += steps[i];
        aud_service(t);
    }
    u32 w = words_at(t - t0);
    CHECK(shim_audio.stat.tx_words == w, "consumed %u words, clock says %u", shim_audio.stat.tx_words, w);
    CHECK(w > MAX_ITEM && w < 3 * MAX_ITEM, "test assumes 100 ms spans LLIs (%u)", w);
    /* registers look like a PL080 that is inside the 3rd item */
    u32 item = w / MAX_ITEM, into = w % MAX_ITEM;
    CHECK(CH(0, SRC) == BUF_PA + w * 2, "SrcAddr %x want %x", CH(0, SRC), BUF_PA + w * 2);
    CHECK((CH(0, CTL) & 0xFFF) == MAX_ITEM - into, "TransferSize %u", CH(0, CTL) & 0xFFF);
    CHECK(CH(0, LLI) == LLI_PA + (item + 1) * 16, "LLI reg %x", CH(0, LLI));
    CHECK(CH(0, CFG) & 1, "channel still enabled");
    CHECK(aud_host_dmac[0] == 0 && !aud_test_inject(), "no interrupt yet");
    /* samples are de-interleaved LEAD frames ahead in the CSND rings */
    int16_t *l = pcm(), *r = l + AUD_RING_FRAMES;
    for (u32 f = 0; f < w / 2; f += 97) {
        u32 pos = (AUD_LEAD_FRAMES + f) & (AUD_RING_FRAMES - 1);
        CHECK(l[pos] == (int16_t)(f + 1) && r[pos] == (int16_t)(-(int)f - 1), "frame %u: L %d R %d", f, l[pos], r[pos]);
    }
    CHECK(aud_test_wr() == ((AUD_LEAD_FRAMES + w / 2) & (AUD_RING_FRAMES - 1)), "write index");
    CHECK(shim_audio.stat.peak >= w / 2 - 1, "peak %u", shim_audio.stat.peak);

    /* the next deadline is the end of the current item, to the tick */
    u32 valid, next = aud_test_next(&valid);
    CHECK(valid, "deadline set");
    u32 before = shim_audio.stat.tx_words;
    aud_service(next - 1);
    CHECK((CH(0, CTL) & 0xFFF) != 0 && (CH(0, CTL) & 0xFFF) < 16, "one tick early: item not finished (%u left)", CH(0, CTL) & 0xFFF);
    aud_service(next);
    CHECK(CH(0, LLI) == LLI_PA + (item + 2) * 16, "at the deadline the next item is loaded (LLI %x)", CH(0, LLI));
    CHECK(shim_audio.stat.tx_words > before, "advanced");

    /* run to the end of the ring command: TC interrupt, channel off, like the hardware */
    t = next;
    while ((CH(0, CFG) & 1) && t - t0 < 2000000) {
        next = aud_test_next(&valid);
        t = valid ? next : t + 1000;
        aud_service(t);
    }
    CHECK(shim_audio.stat.tx_words == RING_BYTES / 2, "whole ring consumed: %u", shim_audio.stat.tx_words);
    CHECK(!(CH(0, CFG) & 1), "channel disabled at the end of the chain");
    CHECK(CH(0, LLI) == 0 && (CH(0, CTL) & 0xFFF) == 0, "LLI/size zero at the end");
    CHECK(aud_host_dmac[0] == 1 && aud_host_dmac[1] == 1 && aud_host_dmac[5] == 1, "IntStatus/TCStatus/RawTC %x %x %x",
          aud_host_dmac[0], aud_host_dmac[1], aud_host_dmac[5]);
    CHECK(aud_test_inject() == 1, "interrupt owed to the VIC");
    u32 dur = t - t0, ideal = (u32)(((unsigned long long)(RING_BYTES / 2) * AUD_RATE_DIV + 127) / 128);
    CHECK(dur >= ideal && dur <= ideal + 2, "ring took %u ticks, ideal %u", dur, ideal);

    /* the driver's filter: read IntStatus, write it to IntTCClear (0x807d115c) */
    aud_host_dmac[2] = aud_host_dmac[0];
    aud_service(t + 10);
    CHECK(aud_host_dmac[0] == 0 && aud_host_dmac[1] == 0 && aud_host_dmac[2] == 0 && aud_host_dmac[5] == 0, "W1C applied");
    CHECK(shim_audio.stat.stops == 1, "stop seen");
    CHECK(!aud_test_inject(), "no second interrupt");
}

/* Steady state as AppleARMIISAudio runs it: the next ring command is always
   chained before the current one ends, so the channel never stops; one
   interrupt per ring, 371.5 ms (389120 ticks) apart. */
static void test_tx_chained(void)
{
    reset();
    u32 idx = queue_ring(0, BUF_PA, RING_BYTES, 1);
    start_chan(1, 0, TX_CFG);
    u32 t = 5000, t0, irqs = 0, last_irq = 0, valid, next;
    aud_service(t);
    t0 = t;
    for (int ring = 0; ring < 5; ring++) {
        /* chain another command behind the running one (the driver links prev.next) */
        u32 first = idx & 0xFF;
        u32 n = 0, words = RING_BYTES / 2, pa = BUF_PA;
        while (words) {
            u32 k = words > MAX_ITEM ? MAX_ITEM : words, *q = lli((first + n) & 0xFF);
            words -= k;
            q[0] = pa; q[1] = I2S_TX_FIFO; q[2] = 0;
            q[3] = TX_CTL | SI | k | (words ? 0 : IBIT);
            lli((first + n - 1) & 0xFF)[2] = LLI_PA + ((first + n) & 0xFF) * 16;
            pa += k * 2;
            n++;
        }
        idx += n;
        while (!aud_test_inject()) {
            next = aud_test_next(&valid);
            CHECK(valid, "running channel must have a deadline");
            if (!valid) return;
            t = next;
            aud_service(t);
        }
        irqs++;
        if (last_irq) {
            u32 period = t - last_irq;
            CHECK(period >= 389119 && period <= 389121, "ring period %u ticks (32768 words x 1520 / 128 = 389120)", period);
        }
        last_irq = t;
        aud_host_dmac[2] = aud_host_dmac[0];        /* the driver acks */
        CHECK(CH(1, CFG) & 1, "channel keeps running across the ring boundary");
    }
    CHECK(irqs == 5, "irqs %u", irqs);
    CHECK(shim_audio.stat.starts == 1 && shim_audio.stat.stops == 0, "one continuous stream");
    (void)t0;
}

static void test_idle_silence_and_stop(void)
{
    reset();
    int16_t *src = (int16_t *)aud_host_p2v(BUF_PA, RING_BYTES);
    for (u32 i = 0; i < 2 * MAX_ITEM; i++) src[i] = 0x1234;
    lli(0)[0] = BUF_PA; lli(0)[1] = I2S_TX_FIFO; lli(0)[2] = 0; lli(0)[3] = TX_CTL | SI | 1000 | IBIT;
    start_chan(0, 0, TX_CFG);
    u32 t = 100;
    aud_service(t);
    t += 20000;                         /* 19 ms: 1684 words of budget, the item has 1000 */
    aud_service(t);
    CHECK(shim_audio.stat.tx_words == 1000, "tx %u", shim_audio.stat.tx_words);
    int16_t *l = pcm();
    u32 w = words_at(20000);
    CHECK(l[AUD_LEAD_FRAMES + 499] == 0x1234, "last real frame");
    CHECK(l[AUD_LEAD_FRAMES + 500] == 0 && l[AUD_LEAD_FRAMES + w / 2 - 1] == 0, "silence after the stream ended");
    CHECK(aud_test_wr() == AUD_LEAD_FRAMES + w / 2, "sink advanced by the whole step: %u", aud_test_wr());
    CHECK(shim_audio.stat.state & 4, "CSND still running");
    /* stale ring contents get overwritten with silence while idle */
    for (u32 i = 0; i < AUD_RING_FRAMES * 2; i++) l[i] = 0x7777;
    for (int i = 0; i < 400; i++) { t += 17000; aud_service(t); }      /* 6.5 s */
    CHECK(!(shim_audio.stat.state & 4), "CSND stopped after 3 s of silence");
    CHECK(csnd16(CSND_CH(0) + CSND_CH_CNT) == 0 && csnd16(CSND_CH(1) + CSND_CH_CNT) == 0, "channels stopped");
    u32 bad = 0;
    for (u32 i = 0; i < AUD_RING_FRAMES * 2; i++) bad += l[i] != 0;
    CHECK(bad == 0, "%u stale samples left in the rings", bad);
    u32 valid;
    aud_test_next(&valid);
    CHECK(!valid, "no deadline while idle");
}

static void test_rx_and_other_channels(void)
{
    reset();
    u8 *buf = (u8 *)aud_host_p2v(BUF_PA, RING_BYTES);
    memset(buf, 0xEE, RING_BYTES);
    /* input stream: I2S RX FIFO -> memory */
    lli(0)[0] = I2S_RX_FIFO; lli(0)[1] = BUF_PA; lli(0)[2] = 0; lli(0)[3] = TX_CTL | DI | 2000 | IBIT;
    start_chan(1, 0, RX_CFG);
    /* a UART transmit on channel 0: memory -> some other peripheral, 8-bit */
    lli(1)[0] = BUF_PA + 0x8000; lli(1)[1] = 0x3CC00020; lli(1)[2] = 0; lli(1)[3] = SI | 100 | IBIT;
    start_chan(0, 1, 0x00000A00);
    /* a UART receive on channel 2: must just sit there */
    lli(2)[0] = 0x3CC00024; lli(2)[1] = BUF_PA + 0x9000; lli(2)[2] = 0; lli(2)[3] = DI | 64 | IBIT;
    start_chan(2, 2, 0x00001000);

    u32 t = 50;
    aud_service(t);
    CHECK(!(CH(0, CFG) & 1) && (aud_host_dmac[1] & 1), "non-audio transmit completes at once with its TC");
    CHECK(CH(0, SRC) == BUF_PA + 0x8000 + 100, "8-bit source advanced by 100: %x", CH(0, SRC));
    CHECK((CH(2, CFG) & 1) && (CH(2, CTL) & 0xFFF) == 64, "receive without a peripheral waits");
    CHECK(aud_host_dmac[7] == 6, "EnbldChns %x", aud_host_dmac[7]);
    aud_test_inject();
    aud_host_dmac[2] = 1;
    t += 11875;                         /* exactly 1000 words */
    aud_service(t);
    CHECK(shim_audio.stat.rx_words == 1000, "rx %u", shim_audio.stat.rx_words);
    CHECK(buf[0] == 0 && buf[1999] == 0, "1000 words of silence delivered (the mic ring is empty)");
    CHECK(CH(1, DST) == BUF_PA + 2000, "DestAddr %x", CH(1, DST));
    CHECK(shim_audio.stat.starts == 0 && !(shim_audio.stat.state & 4), "input alone does not start CSND");
    t += 11875;
    aud_service(t);
    CHECK(!(CH(1, CFG) & 1) && (aud_host_dmac[1] & 2), "input item done, TC on channel 1");
    CHECK(aud_test_inject(), "interrupt");
}

/* microphone: the shim drains the MIC FIFO into its ring and feeds the RX stream */
static void test_mic(void)
{
    reset();
    hmic_cnt = hmic_head = hmic_tail = hmic_next = 0;
    volatile aud_mic_t *m = (volatile aud_mic_t *)aud_host_p2v(AUD_MIC_PA, sizeof(aud_mic_t));
    int16_t *buf = (int16_t *)aud_host_p2v(BUF_PA, RING_BYTES);
    u32 t = 100, bad = 0, steps = 0;
#define G(x) ((int16_t)(((int32_t)(x) * (int32_t)AUD_MIC_GAIN_Q15 + (1 << 14)) >> 15))
    lli(0)[0] = I2S_RX_FIFO; lli(0)[1] = BUF_PA; lli(0)[2] = 0; lli(0)[3] = TX_CTL | DI | 4000 | IBIT;
    start_chan(1, 0, RX_CFG);
    aud_service(t);
    CHECK(m->magic == AUD_MIC_MAGIC && m->want == 1 && m->state == AUD_MIC_RUNNING, "input stream -> mic on");
    CHECK((hmic_cnt & MIC_CNT_EN) && ((hmic_cnt >> 2) & 3) == AUD_MIC_DIV, "MIC_CNT %x", hmic_cnt);
    CHECK(m->rate == 11901, "rate %u", m->rate);
    for (int i = 0; i < 48; i++) {      /* 1 ms services, ~12 mic samples each, as on the device */
        hmic_push(12);
        aud_service(t += 1047);
    }
    CHECK(m->wr >= 560 && m->wr <= 580, "the shim drained the FIFO: %u samples", m->wr);
    CHECK(!(CH(1, CFG) & 1), "4000-word item done");
    /* the first 20 ms are the reader's lag (the ring's old contents); then the ramp */
    for (u32 f = 1000; f < 2000; f++) {
        if (buf[2 * f] != buf[2 * f + 1]) bad++;
        int d = buf[2 * f] - buf[2 * f - 2];
        if (d == 1) steps++;
        else if (d) bad++;
    }
    CHECK(!bad, "%u frames not a scaled, interpolated ramp", bad);
    CHECK(steps > 70 && shim_audio.stat.mic_samples >= 520 && shim_audio.stat.mic_samples <= 545,
          "11901 Hz over 2000 frames at 44097 Hz: %u mic samples used, %u steps", shim_audio.stat.mic_samples, steps);
    CHECK(shim_audio.stat.mic_underruns == 0, "no underruns: %u", shim_audio.stat.mic_underruns);

    /* the input stream pauses briefly between commands: the mic stays on */
    CH(1, CFG) = 0;
    aud_service(t += 100);
    CHECK(m->want == 1 && m->state == AUD_MIC_RUNNING, "short gap: mic stays on");

    /* the MIC block stops delivering: after 250 ms the input goes silent, once */
    lli(1)[0] = I2S_RX_FIFO; lli(1)[1] = BUF_PA; lli(1)[2] = 0; lli(1)[3] = TX_CTL | DI | 4000 | IBIT;
    start_chan(1, 1, RX_CFG);
    for (int i = 0; i < 40; i++) aud_service(t += 10000);
    CHECK(m->state == AUD_MIC_FAILED && m->why == 1 && !(hmic_cnt & MIC_CNT_EN), "no data -> failed, MIC off");
    CHECK(strstr(logbuf, "no samples for 250 ms") != NULL, "logged");
    lli(2)[0] = I2S_RX_FIFO; lli(2)[1] = BUF_PA; lli(2)[2] = 0; lli(2)[3] = TX_CTL | DI | 100 | IBIT;
    start_chan(1, 2, RX_CFG);
    aud_service(++t);
    aud_service(t += 1200);
    CHECK(buf[0] == 0 && buf[99] == 0, "failed mic -> silence");
    CH(1, CFG) = 0;
    aud_service(t += 100);
    aud_service(t += AUD_MIC_HOLD_TICKS);
    CHECK(m->want == 0, "input stream gone for a second -> mic not wanted");

    /* a stream of odd length must not shift the next stream's frames:
       every frame iOS reads has L = R */
    {
        u32 k, mism = 0;
        reset();
        hmic_cnt = hmic_head = hmic_tail = hmic_next = 0;
        lli(0)[0] = I2S_RX_FIFO; lli(0)[1] = BUF_PA + 0x8000; lli(0)[2] = 0; lli(0)[3] = TX_CTL | DI | 1001 | IBIT;
        start_chan(1, 0, RX_CFG);
        aud_service(t = 100);
        for (int i = 0; i < 20; i++) { hmic_push(12); aud_service(t += 1047); }
        CHECK(!(CH(1, CFG) & 1), "odd stream done");
        for (k = 0; k < 4000; k++) buf[k] = 0x5A5A;
        lli(1)[0] = I2S_RX_FIFO; lli(1)[1] = BUF_PA; lli(1)[2] = 0; lli(1)[3] = TX_CTL | DI | 2000 | IBIT;
        start_chan(1, 1, RX_CFG);
        aud_service(t += 1);
        for (int i = 0; i < 30; i++) { hmic_push(12); aud_service(t += 1047); }
        for (k = 0; k < 1000; k++) if (buf[2 * k] != buf[2 * k + 1]) mism++;
        CHECK(!mism, "frames after an odd-length stream: %u with L != R", mism);
    }

    /* boot.cfg mic=0 */
    reset();
    hmic_cnt = 0;
    m->arm9_magic = AUD_MIC_MAGIC;
    m->disabled = 1;
    lli(0)[0] = I2S_RX_FIFO; lli(0)[1] = BUF_PA; lli(0)[2] = 0; lli(0)[3] = TX_CTL | DI | 4000 | IBIT;
    start_chan(1, 0, RX_CFG);
    aud_service(t = 100);
    CHECK(m->state == AUD_MIC_DISABLED && !(hmic_cnt & MIC_CNT_EN), "mic=0: MIC block untouched");
}

static void test_bad_addresses_and_halt(void)
{
    reset();
    /* source outside RAM: plays silence, still keeps time and completes */
    lli(0)[0] = 0x10000000; lli(0)[1] = I2S_TX_FIFO; lli(0)[2] = 0x30000000; lli(0)[3] = TX_CTL | SI | 500;
    start_chan(3, 0, TX_CFG);
    u32 t = 10;
    aud_service(t);
    t += 11875;
    aud_service(t);
    CHECK(shim_audio.stat.tx_words == 500 && shim_audio.stat.bad_addr >= 2, "words %u bad %u", shim_audio.stat.tx_words, shim_audio.stat.bad_addr);
    CHECK(!(CH(3, CFG) & 1), "chain to nowhere ends the channel");
    CHECK(!aud_test_inject(), "no I bit, no interrupt");
    /* halted channel does not move */
    lli(0)[0] = BUF_PA; lli(0)[2] = 0; lli(0)[3] = TX_CTL | SI | 500 | IBIT;
    start_chan(3, 0, TX_CFG | (1u << 18));
    u32 before = shim_audio.stat.tx_words;
    t += 50000;
    aud_service(t);
    CHECK(shim_audio.stat.tx_words == before && (CH(3, CTL) & 0xFFF) == 500, "halt bit respected");
    /* a self-referencing zero-length chain must not hang the IRQ */
    lli(4)[0] = BUF_PA; lli(4)[1] = I2S_TX_FIFO; lli(4)[2] = LLI_PA + 4 * 16; lli(4)[3] = TX_CTL | SI;
    start_chan(3, 4, TX_CFG);
    aud_service(t + 100);
    aud_service(t + 200);
    CHECK(1, "returned");
}

static void test_csnd_resync(void)
{
    reset();
    queue_ring(0, BUF_PA, RING_BYTES, 1);
    start_chan(0, 0, TX_CFG);
    u32 t = 0;
    aud_service(t);
    /* real CSND: position register counts; pretend it runs 100 frames late */
    for (int i = 0; i < 20; i++) {
        t += 17455;
        u32 played = (words_at(t) / 2 + AUD_RING_FRAMES - 100) & (AUD_RING_FRAMES - 1);
        u32 pos = AUD_PCM_PA + played * 2;
        memcpy(aud_host_csnd + CSND_CH(0) + CSND_CH_ADDR, &pos, 4);
        aud_service(t);
    }
    CHECK(shim_audio.stat.csnd_resyncs == 0, "small phase error is left alone (%u)", shim_audio.stat.csnd_resyncs);
    /* now CSND is far away from where we write: realign once */
    u32 pos = AUD_PCM_PA + ((aud_test_wr() + 3000) & (AUD_RING_FRAMES - 1)) * 2;
    memcpy(aud_host_csnd + CSND_CH(0) + CSND_CH_ADDR, &pos, 4);
    t += 100;
    aud_service(t);
    CHECK(shim_audio.stat.csnd_resyncs == 1, "resync %u", shim_audio.stat.csnd_resyncs);
    CHECK(((aud_test_wr() - (pos - AUD_PCM_PA) / 2) & (AUD_RING_FRAMES - 1)) == AUD_LEAD_FRAMES, "lead restored");
}

static void test_iic(void)
{
    reset();
    int codec, amp;
    u8 sub = 0x8F, data[4] = { 0x11, 0x22, 0x33, 0x44 }, rd[4] = { 9, 9, 9, 9 };
    CHECK(shim_iic_transfer(&codec, 2, 1, &sub, 4, data) == 0, "write ok");
    sub = 0x10;
    CHECK(shim_iic_transfer(&codec, 1, 1, &sub, 2, rd) == 0 && rd[0] == 0x22 && rd[1] == 0x33, "read back %x %x", rd[0], rd[1]);
    CHECK(shim_iic_transfer(&amp, 1, 1, &sub, 1, rd) == 0 && rd[0] == 0, "other device has its own registers");
    CHECK(shim_iic_transfer(&codec, 1, 1, NULL, 1, rd) != 0, "bad argument rejected");
    CHECK(shim_iic_transfer(&codec, 1, 1, &sub, 0, rd) != 0, "zero length rejected");
    CHECK(shim_audio.stat.iic_writes == 1 && shim_audio.stat.iic_reads == 2, "counters");

    /* LM48821: one byte, no sub-address, step << 3 | 3 -> CSND volume */
    {
        u8 v = 31 << 3 | 3;
        CHECK(shim_iic_transfer(&amp, 2, 0, NULL, 1, &v) == 0, "amp write ok");
        CHECK(shim_audio.stat.amp_step == 31 && shim_audio.stat.volume == 0x8000, "step 31 clamps to unity: %x",
              shim_audio.stat.volume);
        v = 16 << 3 | 3;                                    /* -6 dB: 6 dB below the reference step 19 */
        shim_iic_transfer(&amp, 2, 0, NULL, 1, &v);
        CHECK(shim_audio.stat.volume > 16300 && shim_audio.stat.volume < 16500, "-6 dB: %u", shim_audio.stat.volume);
        v = 4;                                              /* the driver's mute: mode byte, amp off */
        shim_iic_transfer(&amp, 2, 0, NULL, 1, &v);
        CHECK(shim_audio.stat.volume == 0, "mute");
        v = 11 << 3 | 3;                                    /* back on at the quietest step iOS used (-16 dB) */
        shim_iic_transfer(&amp, 2, 0, NULL, 1, &v);
        CHECK(shim_audio.stat.volume == 5195, "step 11: %u", shim_audio.stat.volume);
        v = 0x22;                                           /* another register of the amp: no volume change */
        shim_iic_transfer(&amp, 2, 0, NULL, 1, &v);
        CHECK(shim_audio.stat.volume == 5195, "non-volume byte ignored");
    }
}

int main(void)
{
    CHECK(sizeof(aud_cfg_t) == 40 && sizeof(aud_stat_t) == 84, "layout shared with mkboot / the ARM9");
    test_tx_stream();
    test_tx_chained();
    test_idle_silence_and_stop();
    test_rx_and_other_channels();
    test_bad_addresses_and_halt();
    test_csnd_resync();
    test_iic();
    test_mic();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
