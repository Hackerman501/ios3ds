/*
 * ios3ds ARM9: the 3DS CODEC's sound side. iOS's own audio drivers run
 * unmodified against emulated S5L8720 hardware; the kernel shim
 * (kernel/shim/audio.c, ARM11) moves the PCM into two CSND channels. CSND
 * output reaches the speaker/headphones through I2S1 and the CTR CODEC, which
 * nobody has initialised after a plain FIRM boot. This does it, over the same
 * SPI bus as the touch panel (boot/arm9/tsc.c), before XNU is started.
 *
 * Sequence = libn3ds source/arm11/drivers/codec.c CODEC_init() + soundInit()
 * (register names from include/arm11/drivers/codec_regmap.h, calibration
 * defaults from source/arm11/drivers/hw_cal.c), minus the microphone,
 * shutter-sound and HWCAL filter coefficients (the CODEC's reset-default
 * filters pass audio through) and minus the o3DS de-pop GPIO. I2S1 runs at
 * 47605 Hz instead of libn3ds' 32 kHz (that is for GBA audio); CSND resamples
 * any channel rate to it. See docs/audio.md.
 *
 * Registers outside the CODEC: PDN_I2S_CNT (0x10141220), I2S1/2_CNT
 * (0x10145000/2) and GPIO2 (0x10147010). libn3ds touches them from the ARM11;
 * the ARM11 shim writes I2S1_CNT/PDN again when it starts CSND, in case the
 * ARM9 cannot reach them on hardware.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"
#include "tsc.h"
#include "log.h"
#include "../../kernel/shim/audio.h"

extern void waitcycles(u32 n);
static void ms(u32 n) { while (n--) waitcycles(33500); }     /* ~1 ms at 134 MHz */

#define GPIO2_DAT   (*(vu8 *)0x10147010u)   /* bit 0: headphones plugged (libn3ds gpio.h GPIO_2_0) */

/* (page, reg) of libn3ds CDC_REG_* */
#define P0   0
#define P100 100
#define P101 101

static bool wait_clear(u8 page, u8 reg, u8 mask, u32 tries)  /* until (~reg & mask) == 0 */
{
    for (u32 i = 0; i < tries && !cdc_bus_dead(); i++) {
        if (!(~cdc_read8(page, reg) & mask)) return true;
        ms(1);
    }
    return false;
}

static bool codec_ok;   /* audio_init found the CODEC: jack polling may write its registers */
static u8 jack;         /* headphone state the CODEC is routed for */
static bool hp_route = true;    /* boot.cfg hp_route: tell iOS about the jack (kernel/shim/audio.c hp_service) */

/* speaker or headphones, as libn3ds soundInit selects them (CDC_REG_HEADSET_SEL) */
static void route(u8 hp)
{
    cdc_mask(P100, 69, (u8)(hp << 4 | 0x20), 0x30);
    cdc_mask(P100, 67, 0, 0x80);
    cdc_mask(P100, 67, 0x80, 0x80);
    jack = hp;
}

bool audio_init(void)
{
    cdc_bus_init();

    /* CODEC MCLK on (only clock 2 is wired to it), then soft reset */
    *(vu8 *)PDN_I2S_CNT_PA = PDN_I2S_CLK2_EN;
    cdc_write(P100, 1, 1);                      /* CDC_REG_SOFT_RST_CTR */
    ms(40);
    cdc_forget_page();
    u8 id = cdc_read8(P0, 2), rev = cdc_read8(P0, 3);
    if (cdc_bus_dead() || id == 0xFF) {
        log_warn("[audio] no CODEC on SPI bus 2 (id %02x): no sound\n", id);
        return false;
    }

    cdc_write(P100, 67, 0x11);                  /* headset detection timing */
    cdc_mask(P101, 119, 1, 1);
    cdc_mask(P0, 57, 0x66, 0x66);               /* GPI1/GPI2: don't force the speaker */
    cdc_write(P101, 122, 1);                    /* VREF */
    cdc_mask(P100, 34, 0x18, 0x18);             /* PLL */

    /* headphone/speaker selection follows the jack (CDC_REG_HEADSET_SEL);
       audio_poll repeats it when the jack changes */
    u8 hp = GPIO2_DAT & 1;
    route(hp);

    /* CODEC-side I2S dividers: I2S1 47.6 kHz (NDAC 0x85), I2S2 47.6 kHz */
    cdc_write(P0, 11, 0x85);
    cdc_mask(P100, 124, 0, 1);

    /* I2S interfaces: clear the enables first, bits 12-14 are locked while set */
    *(vu16 *)I2S1_CNT_PA = 0;
    *(vu16 *)I2S2_CNT_PA = 0;
    *(vu16 *)I2S1_CNT_PA = (u16)AUD_I2S1_CNT;
    *(vu16 *)I2S2_CNT_PA = (u16)AUD_I2S2_CNT;

    cdc_mask(P101, 17, 0x10, 0x1C);             /* speaker driver power-up time */
    cdc_write(P100, 122, 0);                    /* I2S1 volume */
    cdc_write(P100, 120, 0);                    /* I2S2 volume */

    /* DACs on */
    cdc_mask(P100, 118, 0xC0, 0xC0);
    ms(10);
    bool dac = wait_clear(P100, 37, 0x88, 100);
    cdc_write(P101, 10, 0x0A);                  /* route the DACs */
    cdc_mask(P0, 63, 0xC0, 0xC0);               /* unmute I2S1 DAC (CDC_REG_DAC_DATA_PATH_SETUP) */
    cdc_write(P0, 64, 0);                       /* CDC_REG_DAC_VOLUME_CTRL */
    cdc_mask(P100, 119, 0, 0x0C);               /* unmute I2S2 DAC */

    /* headphone driver (gain 0 dB, analog volume 0 dB: hw_cal.c defaults) */
    cdc_write(P101, 11, ((id & 0xF) <= 1 && ((rev & 0x70) >> 4) <= 2) ? 0x3C : 0x1C);
    cdc_write(P101, 12, (0 << 3) | 4);
    cdc_write(P101, 22, 0);
    cdc_write(P101, 23, 0);
    /* speaker driver (gain 12 dB, analog volume -3.5 dB: hw_cal.c defaults) */
    cdc_mask(P101, 17, 0xC0, 0xC0);
    cdc_write(P101, 18, (1 << 2) | 2);
    cdc_write(P101, 19, (1 << 2) | 2);
    cdc_write(P101, 27, 7);
    cdc_write(P101, 28, 7);
    ms(38);

    /* microphone side: libn3ds microphoneInit() without the calibration
       filters (the CODEC's reset defaults pass audio through). The ADC itself
       is powered only while iOS records (mic_start). */
    cdc_mask(P100, 34, 0, 4);                   /* mic ADC output select */
    cdc_mask(1, 48, 0x40, 0xC0);                /* ADC input impedance, P and M terminal */
    cdc_mask(1, 49, 0x40, 0xC0);
    cdc_write(P101, 51, 3);                     /* mic bias (hw_cal default) */
    cdc_mask(P101, 65, 0, 0x3F);                /* PGA gain correction 0 */
    cdc_mask(P101, 66, 2, 3);                   /* quick charge (hw_cal default) */
    cdc_write(1, 47, 43);                       /* mic PGA */

    codec_ok = true;
    log_ok("[audio] CODEC id %02x rev %02x: DAC %s, %s, I2S1_CNT %04x\n", id, rev,
           dac ? "up" : "not confirmed", hp ? "headphones" : "speaker", *(vu16 *)I2S1_CNT_PA);
    return true;
}

#define T9_VAL(n)   (*(vu16 *)(0x10003000u + (n) * 4))   /* main.c's wall clock: 65457 Hz */
#define T9_HZ       65457u
static u32 t9_ticks(void)
{
    u16 hi, lo, hi2;
    do {
        hi = T9_VAL(3);
        lo = T9_VAL(2);
        hi2 = T9_VAL(3);
    } while (hi != hi2);
    return (u32)hi << 16 | lo;
}

/* Headphones plugged or pulled while iOS runs: switch the CODEC's output the
   same way audio_init chose it, and tell iOS (aud_mic_t.jack -> the shim's
   hp_service -> function-hp_detect + jack interrupt), which then switches its
   route: speaker/headphone volume memory, pause on unplug. The jack must read
   the same for JACK_STABLE polls in a row before it counts.
   Unverified on hardware, so iOS is only told when it looks right:
   - GPIO2 bit 0 = jack, 1 = plugged (libn3ds GPIO_2_HEADPH_JACK). Hardware
     boots log "speaker" with nothing plugged (bit 0 reads 0). If it reads 1
     at boot, iOS stays on the speaker route until the jack has read 0 once.
   - the bit only moves with the plug: more than JACK_FLAP_MAX changes within
     JACK_FLAP_S seconds and iOS stays on the speaker route until reboot.
   Each change also logs the CODEC's own headset detection (page 100 reg 67,
   TI layout: bits 6-5 = type detected) so the logs can confirm the bit. */
#define JACK_STABLE 32
#define JACK_FLAP_MAX 8
#define JACK_FLAP_S 10
static u8 ios_jack;                 /* what iOS is told */
static bool jack_armed, jack_flaps; /* seen unplugged once / bit judged unreliable */

static void jack_poll(void)
{
    static u32 same, flap_t0, flaps;
    static u8 last;
    static bool first = true;
    u8 hp = GPIO2_DAT & 1;
    if (first) {                                /* boot: the jack as audio_init saw it */
        first = false;
        if (!codec_ok) jack = hp;
        last = jack;
        jack_armed = !jack;
        if (jack) log_warn("[audio] jack: GPIO2 bit 0 reads 'plugged' at boot: iOS stays on the speaker "
                           "route until it reads 'unplugged' once (polarity unverified)\n");
    }
    if (hp != last) {
        last = hp;
        same = 0;
        return;
    }
    if (hp == jack || ++same < JACK_STABLE) return;
    if (codec_ok && !cdc_bus_dead()) route(hp);
    else jack = hp;
    if (!hp) jack_armed = true;
    if (!flaps || t9_ticks() - flap_t0 > JACK_FLAP_S * T9_HZ) {
        flap_t0 = t9_ticks();
        flaps = 0;
    }
    if (++flaps > JACK_FLAP_MAX && !jack_flaps) {
        jack_flaps = true;
        log_warn("[audio] jack: GPIO2 bit 0 changed %u times in %u s: not a plug? iOS stays on the speaker "
                 "route until reboot\n", (unsigned)flaps, JACK_FLAP_S);
    }
    ios_jack = hp && jack_armed && !jack_flaps;
    if (jack_flaps) return;                     /* logged once above */
    log_ok("[audio] headphones %s: CODEC output -> %s, iOS route -> %s (CODEC headset detect %02x)\n",
           hp ? "plugged in" : "unplugged", hp ? "headphones" : "speaker",
           !hp_route ? "speaker (hp_route=0)" : ios_jack ? "headphones" : "speaker",
           codec_ok && !cdc_bus_dead() ? cdc_read8(P100, 67) : 0xFF);
}

/* ---- microphone -------------------------------------------------------------
   The kernel shim drains the MIC FIFO itself (ARM11, kernel/shim/audio.c) while
   iOS records and says so in aud_mic_t.want; the ARM9 owns the CODEC (SPI), so
   it powers the ADC for that time, publishes boot.cfg mic=0 and logs. */
static volatile aud_mic_t *const micr = (volatile aud_mic_t *)AUD_MIC_PA;
static struct { bool enabled, adc; u32 state; } mic = { .enabled = true, .state = ~0u };

void audio_set_mic(bool enabled) { mic.enabled = enabled; }
void audio_set_hp_route(bool enabled) { hp_route = enabled; }

static void mic_poll(volatile aud_stat_t *st)
{
    static u32 svc0, early0, t0;
    static const char *const why[] = { "?", "no samples for 250 ms", "FIFO never empties",
                                       "MIC block switched itself off", "no IO window" };
    bool want;
    micr->disabled = !mic.enabled;              /* our line: rewritten every pass */
    micr->jack = ios_jack;
    micr->hp_disabled = !hp_route || jack_flaps;
    micr->arm9_magic = AUD_MIC_MAGIC;
    if (micr->magic != AUD_MIC_MAGIC) return;   /* the shim has not used the mic yet */
    want = micr->want && mic.enabled;
    if (want != mic.adc && codec_ok) {
        if (want) {
            cdc_mask(P0, 81, 0x80, 0x80);       /* ADC powered up */
            cdc_mask(P0, 82, 0, 0x80);          /* ADC unmuted */
        } else {
            cdc_mask(P0, 82, 0x80, 0x80);
            cdc_mask(P0, 81, 0, 0x80);
        }
        mic.adc = want;
        micr->adc_on = want;
    }
    if (micr->state != mic.state) {
        mic.state = micr->state;
        if (mic.state == AUD_MIC_RUNNING) {
            svc0 = st ? st->services : 0;
            early0 = st ? st->early_wakes : 0;
            t0 = t9_ticks();
            log_ok("[audio] mic on: %u Hz into iOS's input stream\n", (unsigned)micr->rate);
        } else if (mic.state == AUD_MIC_OFF && micr->starts) {
            u32 ms = (t9_ticks() - t0) / (T9_HZ / 1000);
            log_ok("[audio] mic off: %u samples, %u FIFO overruns; %u shim services (%u early) in %u ms\n",
                   (unsigned)micr->wr, (unsigned)micr->overruns, (unsigned)(st ? st->services - svc0 : 0),
                   (unsigned)(st ? st->early_wakes - early0 : 0), (unsigned)ms);
        }
        else if (mic.state == AUD_MIC_FAILED)
            log_warn("[audio] mic: %s: input stays silent\n", why[micr->why < 5 ? micr->why : 0]);
        else if (mic.state == AUD_MIC_DISABLED)
            log_warn("[audio] mic disabled (boot.cfg mic=0): input stays silent\n");
    }
}

void audio_poll(u32 shim_pa)
{
    static volatile aud_blk_t *b;
    static bool searched;
    static u32 starts, stops, csnd, step = ~0u, hp_state, hp_vol = AUD_HPVOL_NONE;

    jack_poll();
    if (!shim_pa) return;
    if (!searched) {                            /* aud_blk_t lives in the shim's .data */
        searched = true;
        for (u32 o = 0; o + sizeof(aud_blk_t) <= SHIM_BLK_OFF; o += 4) {
            vu32 *w = (vu32 *)(shim_pa + o);
            if (w[0] == AUD_CFG_MAGIC0 && w[1] == AUD_CFG_MAGIC1) { b = (volatile aud_blk_t *)w; break; }
        }
    }
    if (!b) { mic_poll(NULL); return; }
    volatile aud_stat_t *s = &b->stat;
    mic_poll(s);
    if (s->starts != starts) {
        starts = s->starts;
        log_ok("[audio] iOS output stream #%u started (CSND starts %u)\n", (unsigned)starts, (unsigned)s->csnd_starts);
    }
    if (s->stops != stops) {
        stops = s->stops;
        log_ok("[audio] stream stopped: %u words, peak %u, TC IRQs %u (%u injected), bad addr %u\n",
               (unsigned)s->tx_words, (unsigned)s->peak, (unsigned)s->tc_irqs, (unsigned)s->injected,
               (unsigned)s->bad_addr);
    }
    if (s->amp_step != step && s->amp_step != AUD_AMP_NONE) {
        step = s->amp_step;
        if (step == AUD_AMP_OFF) log_ok("[audio] iOS volume: muted (amp off) -> CSND volume 0\n");
        else log_ok("[audio] iOS volume: amp step %u -> CSND volume %u/32768\n", (unsigned)step, (unsigned)s->volume);
    }
    if (s->hp_state != hp_state) {                /* the shim told iOS (kernel/shim/audio.c hp_service) */
        hp_state = s->hp_state;
        log_ok("[audio] iOS route -> %s (jack change #%u passed to iOS), CSND volume %u/32768\n",
               hp_state == 1 ? "headphones" : hp_state == AUD_HP_OFF ? "speaker for good (hp_route=0 or jack unreliable)"
                                                                     : "speaker",
               (unsigned)s->hp_changes, (unsigned)s->volume);
    }
    if (s->hp_vol != hp_vol && hp_state == 1) {
        hp_vol = s->hp_vol;
        if (hp_vol & 0x80) log_ok("[audio] iOS headphone volume: muted (CS42L58 %02x) -> CSND volume 0\n", (unsigned)hp_vol);
        else log_ok("[audio] iOS headphone volume: CS42L58 %02x (%d dB) -> CSND volume %u/32768\n", (unsigned)hp_vol,
                    (int)(hp_vol & 0x7F) - ((hp_vol & 0x40) ? 0x80 : 0), (unsigned)s->volume);
    }
    if (s->csnd_starts != csnd) {
        csnd = s->csnd_starts;
        if (!(b->cfg.flags & AUD_F_CSND)) log_warn("[audio] CSND disabled in the shim config\n");
    }
}
