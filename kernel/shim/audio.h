/*
 * ios3ds audio: definitions shared by the kernel shim (kernel/shim/audio.c), the ARM9
 * monitor (boot/arm9/audio.c), scripts/mkboot.py (patches aud_cfg_t by magic)
 * and the host tests. See docs/audio.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* ---- configuration block inside shim.bin, located by its two magic words.
   mkboot fills dmac_pa / i2s_pa (fake S5L8720 register pages, kernel/dtgen.py);
   dmac_pa == 0 disables everything. ---------------------------------------- */
#define AUD_CFG_MAGIC0  0x30445541u     /* 'AUD0' */
#define AUD_CFG_MAGIC1  0x21676663u     /* 'cfg!' */
#define AUD_F_CSND      (1u << 0)       /* drive the 3DS CSND block */
#define AUD_F_LOG       (1u << 1)       /* trace stream starts/stops and I2C writes into the XNU log ring */

typedef struct {
    uint32_t magic0, magic1;
    uint32_t dmac_pa;       /* fake PL080 page (PA) */
    uint32_t i2s_pa;        /* fake S5L I2S page (PA), informational */
    uint32_t pcm_pa;        /* CSND sample memory (PA, FCRAM): L ring then R ring */
    uint32_t ring_frames;   /* frames per CSND ring (power of two) */
    uint32_t lead_frames;   /* how far ahead of CSND's play position we write */
    uint32_t rate_div;      /* CSND timer divider: Fs = 67027964 / rate_div (1520 -> 44097.3 Hz) */
    uint32_t flags;         /* AUD_F_* */
    uint32_t stat_off;      /* offset of the aud_stat_t that follows, from the first magic (bytes) */
} aud_cfg_t;

/* ---- statistics, written by the shim, read by the ARM9 monitor (the shim
   cleans the cache lines after every update that matters) ------------------ */
typedef struct {
    uint32_t services;      /* emulator runs (one per IRQ taken) */
    uint32_t starts;        /* I2S TX DMA channel went idle -> running */
    uint32_t stops;
    uint32_t tx_words;      /* 16-bit samples consumed from iOS's ring */
    uint32_t rx_words;      /* 16-bit samples of silence delivered to the input stream */
    uint32_t tc_irqs;       /* terminal-count interrupts raised */
    uint32_t injected;      /* ... and delivered through the fake VIC */
    uint32_t early_wakes;   /* private-timer IRQs taken only for audio */
    uint32_t csnd_starts;
    uint32_t csnd_resyncs;  /* write pointer re-aligned to CSND's position register */
    uint32_t bad_addr;      /* DMA addresses outside FCRAM (skipped) */
    uint32_t iic_writes, iic_reads;
    uint32_t peak;          /* max |sample| since the last start: nonzero = iOS really plays something */
    uint32_t last_src;      /* last source PA consumed */
    uint32_t state;         /* bit0 TX running, bit1 RX running, bit2 CSND running, bit3 IO window mapped */
    uint32_t amp_step;      /* last LM48821 gain step iOS wrote (0..31), AUD_AMP_OFF, AUD_AMP_NONE */
    uint32_t volume;        /* CSND channel volume derived from it (0..0x8000) */
} aud_stat_t;

typedef struct {
    aud_cfg_t cfg;
    aud_stat_t stat;
} aud_blk_t;

/* ---- fixed placement (PA) of the CSND sample memory: FCRAM between the XNU
   log ring (ends 0x20890010) and topOfKernelData (0x20900000); like the shim
   and the ring it is mapped by XNU's static map and never handed to the VM. - */
#define AUD_PCM_PA        0x208C0000u
#define AUD_RING_FRAMES   8192u         /* 186 ms at 44.1 kHz; 2 x 16 KiB */
#define AUD_LEAD_FRAMES   2048u         /* 46 ms of output latency */
#define AUD_RATE_DIV      1520u         /* 67027964 / 1520 = 44097.34 Hz */
#define AUD_AMP_NONE      0xFFu         /* aud_stat_t.amp_step: iOS has not set a level yet */
#define AUD_AMP_OFF       0x100u        /* ... the amp was shut down (volume 0) */
#define AUD_AMP_REF_STEP  31u           /* LM48821 step of iOS's loudest setting (+18 dB) = CSND unity (audio.c amp_gain) */

/* ---- 3DS CSND (0x10103000), from libn3ds include/arm11/drivers/csnd.h ----- */
#define CSND_PA           0x10103000u
#define CSND_MASTER_VOL   0x000   /* u16, 0..0x8000 */
#define CSND_CNT          0x002   /* u16: bit15 enable, bit14 resample filter, bit0 mute */
#define CSND_CH(n)        (0x400 + (n) * 0x20)
#define CSND_CH_CNT       0x00    /* u16 */
#define CSND_CH_SR        0x02    /* u16: 0x10000 - 67027964 / Fs */
#define CSND_CH_VOL       0x04    /* u32: R (low 16), L (high 16), 0..0x8000 */
#define CSND_CH_CAPVOL    0x08
#define CSND_CH_ADDR      0x0C    /* start address / current play position (PA) */
#define CSND_CH_SIZE      0x10    /* bytes */
#define CSND_CH_LOOP      0x14    /* loop restart address */
#define CSND_CH_ADPCM0    0x18
#define CSND_CH_ADPCM1    0x1C
#define CSND_CNT_EN           (1u << 15)
#define CSND_CNT_RS_FILTER    (1u << 14)
#define CSND_CH_LERP          (1u << 6)
#define CSND_CH_RPT_LOOP      (1u << 10)
#define CSND_CH_FMT_PCM16     (1u << 12)
#define CSND_CH_PLAYING       (1u << 14)
#define CSND_CH_START         (1u << 15)
#define CSND_CLOCK            67027964u     /* = ARM11 clock / 4 */
/* ---- I2S1 output + CODEC master clock (libn3ds i2s.h, pdn.h); the ARM9
   (boot/arm9/audio.c) and the shim (csnd_start) both write them ------------ */
#define I2S1_CNT_PA       0x10145000u   /* u16 */
#define I2S2_CNT_PA       0x10145002u   /* u16 */
#define PDN_I2S_CNT_PA    0x10141220u   /* u8 */
#define PDN_I2S_CLK2_EN   (1u << 1)     /* MCLK2 -> CODEC */
#define I2S1_EN           (1u << 15)
#define I2S1_MCLK_16MHZ   (1u << 14)
#define I2S1_FREQ_47KHZ   (1u << 13)
/* enabled, 16 MHz MCLK, 47605 Hz, legacy (GBA/DS) volume 32, DSP/CSND volume 32 */
#define AUD_I2S1_CNT      (I2S1_EN | I2S1_MCLK_16MHZ | I2S1_FREQ_47KHZ | (32u << 6) | 32u)
#define AUD_I2S2_CNT      (1u << 15 | 1u << 14 | 1u << 13)  /* I2S2 (mic/camera side) as libn3ds */
#define AUD_CH_L          0
#define AUD_CH_R          1
