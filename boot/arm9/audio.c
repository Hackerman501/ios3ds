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

    /* headphone/speaker selection follows the jack (CDC_REG_HEADSET_SEL) */
    u8 hp = GPIO2_DAT & 1;
    cdc_mask(P100, 69, (u8)(hp << 4 | 0x20), 0x30);
    cdc_mask(P100, 67, 0, 0x80);
    cdc_mask(P100, 67, 0x80, 0x80);

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

    log_ok("[audio] CODEC id %02x rev %02x: DAC %s, %s, I2S1_CNT %04x\n", id, rev,
           dac ? "up" : "not confirmed", hp ? "headphones" : "speaker", *(vu16 *)I2S1_CNT_PA);
    return true;
}

void audio_poll(u32 shim_pa)
{
    static volatile aud_blk_t *b;
    static bool searched;
    static u32 starts, stops, csnd;

    if (!shim_pa) return;
    if (!searched) {                            /* aud_blk_t lives in the shim's .data */
        searched = true;
        for (u32 o = 0; o + sizeof(aud_blk_t) <= SHIM_BLK_OFF; o += 4) {
            vu32 *w = (vu32 *)(shim_pa + o);
            if (w[0] == AUD_CFG_MAGIC0 && w[1] == AUD_CFG_MAGIC1) { b = (volatile aud_blk_t *)w; break; }
        }
    }
    if (!b) return;
    volatile aud_stat_t *s = &b->stat;
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
    if (s->csnd_starts != csnd) {
        csnd = s->csnd_starts;
        if (!(b->cfg.flags & AUD_F_CSND)) log_warn("[audio] CSND disabled in the shim config\n");
    }
}
