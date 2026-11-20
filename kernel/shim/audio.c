/*
 * ios3ds shim: sound. Apple's whole audio stack runs unmodified (IOAudio2Family,
 * AppleEmbeddedAudio + AppleCS42L58Audio, AppleARMIISAudio, AppleARMPL080DMAC);
 * this file is the hardware underneath it:
 *
 *   1. PL080 DMA controller emulation on the fake dmac0 register page. The
 *      Apple driver programs channels and linked-list items (LLIs) exactly as
 *      on an S5L8720; aud_service() "runs" them: the I2S transmit channel is
 *      consumed at the sample clock, the registers advance like the real
 *      controller's, and terminal-count interrupts are raised (delivered
 *      through the fake VIC by audio_glue.S).
 *   2. The samples that leave through the virtual I2S go to the 3DS's CSND
 *      block: two looping PCM16 channels (left/right) over two rings in FCRAM,
 *      written LEAD frames ahead of CSND's play position.
 *   3. I2C: AppleARMIICDevice's transfer method lands in shim_iic_transfer(),
 *      a register file per device (writes are remembered, reads return them),
 *      so the codec and amplifier drivers see a bus that always answers.
 *
 * Clocks: CSND's sample clock is 67027964 Hz / div, the shim's time base is
 * 134055928 Hz / 128 (kernel/shim/entry.S). Both come from the same crystal,
 * so "16-bit words per tick" is exactly 128 / div (two words per frame) and
 * the emulated DMA and CSND never drift apart.
 *
 * Everything here runs either in IRQ mode on the private stack of
 * audio_glue.S (aud_service, shim_audio_irq) or in a kernel thread
 * (shim_iic_transfer, shim_audio_setdec). No libgcc: no '/' or '%' on
 * variables.
 *
 * Facts about the Apple drivers that this code relies on are listed with
 * addresses in docs/audio.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "audio.h"

/* ---- PL080 register map (ARM DDI 0196) ---------------------------------- */
#define DMAC_INT_STATUS     (0x000 / 4)
#define DMAC_INT_TC_STATUS  (0x004 / 4)
#define DMAC_INT_TC_CLEAR   (0x008 / 4)
#define DMAC_INT_ERR_STATUS (0x00C / 4)
#define DMAC_INT_ERR_CLEAR  (0x010 / 4)
#define DMAC_RAW_TC         (0x014 / 4)
#define DMAC_RAW_ERR        (0x018 / 4)
#define DMAC_ENBLD_CHNS     (0x01C / 4)
#define DMAC_CH(n)          ((0x100 + (n) * 0x20) / 4)
#define CH_SRC   0
#define CH_DST   1
#define CH_LLI   2
#define CH_CTL   3
#define CH_CFG   4
#define CTL_SIZE_MASK   0x00000FFFu
#define CTL_SWIDTH(c)   (((c) >> 18) & 7u)      /* 0 = 8, 1 = 16, 2 = 32 bit */
#define CTL_SI          (1u << 26)
#define CTL_DI          (1u << 27)
#define CTL_I           (1u << 31)              /* terminal-count interrupt for this item */
#define CFG_E           (1u << 0)
#define CFG_FLOW(c)     (((c) >> 11) & 7u)      /* 1 = memory -> peripheral, 2 = peripheral -> memory */
#define CFG_ITC         (1u << 15)
#define CFG_HALT        (1u << 18)

/* S5L8720 I2S0 as the device tree's dma-channels name it: TX FIFO 0x3CA00010,
   RX FIFO 0x3CA00038 (the PL080 driver copies these into DestAddr/SrcAddr) */
#define IS_I2S(addr)    (((addr) & 0xFFFFF000u) == 0x3CA00000u)

#define MAX_DT          2000000u    /* ticks: cap for one catch-up step (1.9 s) */
#define IDLE_STOP_WORDS (3u * 2u * 44100u)  /* CSND keeps playing silence this long after iOS stops */
#define LLI_GUARD       600
#define RX_STEP_TICKS   1047        /* 1 ms of the shim time base (1.047 MHz): input service period */

enum { MODE_TX, MODE_RX, MODE_DROP };

/* The ARM9 loads shim.bin without clearing what follows it, so nothing here
   may live in .bss: every object is initialised data. */
#define AUD_DATA __attribute__((section(".data")))

/* Configuration + statistics in one block. scripts/mkboot.py finds it by the
   magic and fills dmac_pa / i2s_pa; the ARM9 monitor finds it the same way and
   reads the statistics that follow the configuration. */
AUD_DATA volatile aud_blk_t shim_audio = { .cfg = {
    .magic0 = AUD_CFG_MAGIC0, .magic1 = AUD_CFG_MAGIC1,
    .pcm_pa = AUD_PCM_PA, .ring_frames = AUD_RING_FRAMES, .lead_frames = AUD_LEAD_FRAMES,
    .rate_div = AUD_RATE_DIV, .flags = AUD_F_CSND | AUD_F_LOG,
    .stat_off = sizeof(aud_cfg_t),
}, .stat = { .amp_step = AUD_AMP_NONE, .volume = 0x8000, .hp_vol = AUD_HPVOL_NONE } };
#define shim_audio_cfg  shim_audio.cfg
#define shim_audio_stat shim_audio.stat

static struct {
    u32 pacing, pace_last, pace_frac;   /* the I2S word clock */
    u32 tx_on, rx_on;                   /* an I2S TX / RX DMA channel was running at the last service */
    u32 csnd_on, idle_words;
    u32 csnd_moved;                     /* CSND's position register was seen counting (real hardware) */
    u32 wr, phase;                      /* ring write index (frames); 0 = next TX word is the left sample */
    u32 inject;                         /* a TC interrupt is raised and not yet delivered */
    u32 next_valid, next;               /* tick of the next audio deadline */
    u32 k_valid, k_deadline, clamped;   /* XNU's own timer deadline; 1 = the private timer runs short for audio */
    u32 io_ok;
    u32 vol, vol_applied;               /* CSND channel volume (0..0x8000): out_vol() */
    u32 amp_vol;                        /* ... from the speaker amp's gain */
    u32 hp_reg, hp_reg_valid;           /* the CS42L58 headphone volume iOS wrote (AUD_HPVOL_REG) */
    u32 mic_want;                       /* last value given to aud_mic_t.want */
    u32 rx_last;                        /* tick of the last service that saw the input stream */
    u32 mic_synced, mic_rd, mic_frac, mic_step, rx_phase;
    u32 mic_on, mic_t_data;
    u32 hp_init, hp_state, hp_inject;   /* headphones as iOS sees them; a jack interrupt to deliver */
    int32_t dbg_prev;
    u32 dbg_jumps, dbg_maxd;            /* RX output: frame-to-frame jumps > 16 and the largest (logged at rx stop) */
    int32_t mic_last;
} A AUD_DATA = { .vol = 0x8000 };

/* ======================================================================== */
#ifndef AUD_HOST_TEST

#define PHYS_BASE   0x20000000u
#define VIRT_BASE   0x80000000u
#define AXI_PA      0x1FF00000u     /* 1 MiB device sections added by kernel/shim/entry.S shim_init */
#define AXI_VA      0xBFD00000u
#define PRIV_VA     0xBFF00000u
#define IO_PA       0x10100000u     /* 3DS "common" IO: CSND at +0x3000 */
#define IO_VA       0xBFC00000u     /* our own section (aud_map_io) */
#define WD_COUNT    0x624
#define PT_LOAD     0x600
#define PT_CTRL     0x608
#define PT_ISR      0x60C
#define GICC_EOIR   0x110
extern u32 shim_pt_ctrl_run, cfg_n3ds;   /* entry.S: timer prescaler, New 3DS tick shift (docs/n3ds.md) */
u32 shim_now32(void);
void shim_l2_inv_kva(const volatile void *va, u32 len);
#define PT_CTRL_RUN shim_pt_ctrl_run
#define TB_SHIFT    ((cfg_n3ds >> 8) & 0xFu)
#define SHIM_CFG_VIC0   0x70        /* entry.S cfg_vic0_pa */

#define REG32(va)   (*(vu32 *)(va))
#define REG16(va)   (*(vu16 *)(va))

static inline u32 now_ticks(void) { return shim_now32(); }   /* 1047311 Hz on every model */
static inline volatile u32 *dmac_regs(void) { return (volatile u32 *)(shim_audio_cfg.dmac_pa - AXI_PA + AXI_VA); }

/* XNU maps all of RAM at VIRT_BASE (static map set up by the entry stub) */
static inline void *p2v(u32 pa, u32 len)
{
    if (pa < PHYS_BASE || pa + len < pa || pa + len > PHYS_BASE + FCRAM_SIZE) return NULL;
    return (void *)(pa - PHYS_BASE + VIRT_BASE);
}

static void dinval(const volatile void *p, u32 len)   /* the ARM9 wrote RAM behind our cache */
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    shim_l2_inv_kva(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
}

static void dclean(const void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static void log_putc(int c) { ((void (*)(int))SHIM_VA)(c); }    /* entry.S +0x00: shim_putc */

/* Map the 3DS IO window that holds CSND. Same recipe as shim_init. */
static void aud_map_io(void)
{
    u32 ttbr1, *l1, want = IO_PA | (1u << 10) | (1u << 4) | (1u << 2) | 2u;
    if (A.io_ok) return;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(ttbr1));
    l1 = (u32 *)((ttbr1 & ~0x3FFFu) - PHYS_BASE + VIRT_BASE) + (IO_VA >> 20);
    if (*l1 != want) {
        if (*l1 != 0) return;       /* someone else owns this megabyte: leave CSND alone */
        *l1 = want;
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(l1) : "memory");
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
        __asm__ volatile("mcr p15, 0, %0, c8, c7, 1" :: "r"(IO_VA) : "memory");
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(0) : "memory");
    }
    A.io_ok = 1;
    shim_audio_stat.state |= 8;
}

#define CSND_REG16(off) REG16(IO_VA + (CSND_PA - IO_PA) + (off))
#define CSND_REG32(off) REG32(IO_VA + (CSND_PA - IO_PA) + (off))
static inline int csnd_usable(void) { return (shim_audio_cfg.flags & AUD_F_CSND) && A.io_ok; }
static inline void csnd_w16(u32 off, u32 v) { CSND_REG16(off) = (u16)v; }
static inline void csnd_w32(u32 off, u32 v) { CSND_REG32(off) = v; }
static inline u32 csnd_r16(u32 off) { return CSND_REG16(off); }
static inline u32 csnd_r32(u32 off) { return CSND_REG32(off); }
static inline volatile u32 *hp_pin_word(void)
{
    return (volatile u32 *)(shim_audio_cfg.gpio_pa - AXI_PA + AXI_VA + AUD_HP_GPIO_OFF);
}
static inline u32 hp_vector(void)                   /* VICVECTADDR12 as AppleARMPL192VIC programmed it */
{
    return REG32(REG32(SHIM_VA + SHIM_CFG_VIC0) - AXI_PA + AXI_VA + 0x100 + AUD_HP_VIC_SRC * 4);
}
static inline u32 mic_cnt_r(void) { return REG16(IO_VA + (MIC_CNT_PA - IO_PA)); }
static inline void mic_cnt_w(u32 v) { REG16(IO_VA + (MIC_CNT_PA - IO_PA)) = (u16)v; }
static inline u32 mic_data_r(void) { return REG32(IO_VA + (MIC_DATA_PA - IO_PA)); }

/* CSND is heard through I2S1 to the CODEC. boot/arm9/audio.c programs the
   CODEC and these two registers before XNU starts; assert them again from the
   ARM11 (libn3ds drives both from the ARM11: codec.c pdnControlMclk/soundInit).
   I2S1_CNT bits 12-14 are only writable while bit 15 is clear (libn3ds i2s.h). */
static void i2s_out_enable(void)
{
    vu16 *i2s1 = (vu16 *)(IO_VA + (I2S1_CNT_PA - IO_PA));
    vu8 *pdn = (vu8 *)(IO_VA + (PDN_I2S_CNT_PA - IO_PA));
    u32 v;
    if (!A.io_ok) return;
    if (!(*pdn & PDN_I2S_CLK2_EN)) *pdn = (u8)(*pdn | PDN_I2S_CLK2_EN);
    v = *i2s1;
    if (!(v & I2S1_EN)) {
        *i2s1 = 0;
        *i2s1 = (u16)AUD_I2S1_CNT;
    } else if ((v & 0x3Fu) == 0) {
        *i2s1 = (u16)((v & ~0x3Fu) | 32u);     /* DSP/CSND volume 100 % */
    }
}

#else   /* ---- host build: tests/test_audio.c provides the machine ---------- */

extern u32 aud_host_dmac[0x400];
extern void *aud_host_p2v(u32 pa, u32 len);
extern int aud_host_csnd_present;
extern u8 aud_host_csnd[0x1000];
extern void aud_host_log(int c);
static inline volatile u32 *dmac_regs(void) { return aud_host_dmac; }
static inline void *p2v(u32 pa, u32 len) { return aud_host_p2v(pa, len); }
static inline void dclean(const void *p, u32 len) { (void)p; (void)len; }
static inline void dinval(const volatile void *p, u32 len) { (void)p; (void)len; }
static void log_putc(int c) { aud_host_log(c); }
static void aud_map_io(void) { A.io_ok = 1; shim_audio_stat.state |= 8; }
static inline int csnd_usable(void) { return (shim_audio_cfg.flags & AUD_F_CSND) && aud_host_csnd_present; }
static inline void csnd_w16(u32 off, u32 v) { aud_host_csnd[off] = (u8)v; aud_host_csnd[off + 1] = (u8)(v >> 8); }
static inline void csnd_w32(u32 off, u32 v) { csnd_w16(off, v); csnd_w16(off + 2, v >> 16); }
static inline u32 csnd_r16(u32 off) { return aud_host_csnd[off] | (u32)aud_host_csnd[off + 1] << 8; }
static inline u32 csnd_r32(u32 off) { return csnd_r16(off) | csnd_r16(off + 2) << 16; }
static void i2s_out_enable(void) { }
extern u32 aud_host_gpio[0x400];
extern u32 aud_host_hp_vector;
static inline volatile u32 *hp_pin_word(void) { return &aud_host_gpio[AUD_HP_GPIO_OFF / 4]; }
static inline u32 hp_vector(void) { return aud_host_hp_vector; }
extern u32 aud_host_mic_cnt_r(void);
extern void aud_host_mic_cnt_w(u32 v);
extern u32 aud_host_mic_data_r(void);
static inline u32 mic_cnt_r(void) { return aud_host_mic_cnt_r(); }
static inline void mic_cnt_w(u32 v) { aud_host_mic_cnt_w(v); }
static inline u32 mic_data_r(void) { return aud_host_mic_data_r(); }

#endif
/* ======================================================================== */

/* ---- tiny logging into the XNU log ring (shows up in sd:/ios3ds/log.txt) -- */
static void log_str(const char *s) { while (*s) log_putc(*s++); }
static void log_hex(u32 v)
{
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        u32 d = (v >> sh) & 15;
        if (d || started || sh == 0) { log_putc((int)(d < 10 ? '0' + d : 'a' + d - 10)); started = 1; }
    }
}
static void log_kv(const char *k, u32 v) { log_str(k); log_hex(v); }
static inline int logging(void) { return (shim_audio_cfg.flags & AUD_F_LOG) != 0; }

/* q = n / d, *r = n % d without libgcc */
static u32 udivmod(u32 n, u32 d, u32 *r)
{
    u32 q = 0;
    if (d == 0) { *r = 0; return 0; }
    for (int i = 31; i >= 0; i--) {
        if ((n >> i) >= d) { n -= d << i; q |= 1u << i; }
    }
    *r = n;
    return q;
}

/* ---- CSND sink ------------------------------------------------------------ */
static inline u32 ring_mask(void) { return shim_audio_cfg.ring_frames - 1; }
static inline int16_t *ring_l(void) { return (int16_t *)p2v(shim_audio_cfg.pcm_pa, shim_audio_cfg.ring_frames * 4); }

static void sink_clean(u32 from, u32 to)        /* frames [from, to) modulo the ring, both channels */
{
    int16_t *l = ring_l();
    u32 n = shim_audio_cfg.ring_frames;
    if (!l || from == to) return;
    if (from < to) {
        dclean(l + from, (to - from) * 2);
        dclean(l + n + from, (to - from) * 2);
    } else {
        dclean(l + from, (n - from) * 2);
        dclean(l + n + from, (n - from) * 2);
        dclean(l, to * 2);
        dclean(l + n, to * 2);
    }
}

static void sink_put(const u16 *src, u32 words)     /* src == NULL: silence */
{
    int16_t *l = ring_l(), *r;
    u32 mask = ring_mask(), wr = A.wr, phase = A.phase, peak = shim_audio_stat.peak;
    if (!l) return;
    r = l + shim_audio_cfg.ring_frames;
    for (u32 i = 0; i < words; i++) {
        int16_t s = src ? (int16_t)src[i] : 0;
        u32 mag = (u32)(s < 0 ? -s : s);
        if (mag > peak) peak = mag;
        if (!phase) {
            l[wr] = s;
        } else {
            r[wr] = s;
            wr = (wr + 1) & mask;
        }
        phase ^= 1;
    }
    A.wr = wr;
    A.phase = phase;
    shim_audio_stat.peak = peak;
}

/* ---- volume ----------------------------------------------------------------
   iOS sets the speaker level on the LM48821 amp, not in the PCM: AppleLM48821Amp
   (0x807b8108) turns a gain in whole dB into a 5-bit step and writes one byte,
   step << 3 | 3, with no sub-address (the codec's HP output feeds the amp, DT
   "hpout-spkr"). The 3DS has no such amp, so the step becomes CSND's channel
   volume: the dB the driver meant, relative to the step of iOS's loudest
   setting (AUD_AMP_REF_STEP = unity; the driver's range is -38..+18 dB,
   0x807b800c). Volume 0 is not a step: the driver's mute (0x807b8048) writes
   the mode byte 0x04 (amp shut down) until the next step.
   amp_gain[s] = 32768 * 10^((dB(s) - 18) / 20), dB(s) = the lower bound of
   the driver's comparison chain (-62 -52 -44 ... 0 2 4 ... 12 13 ... 18). */
static const u16 amp_gain[32] = {
    0, 3, 10, 26, 52, 82, 130, 184, 260, 368, 519, 654, 823, 1036, 1305, 1642,
    2068, 2603, 3277, 4125, 5193, 6538, 8231, 10362, 13045, 16423, 18427, 20675,
    23198, 26029, 29205, 32768,
};

static u32 amp_step_to_vol(u32 step)
{
    u32 r, v = udivmod(amp_gain[step & 31] * 0x8000u, amp_gain[AUD_AMP_REF_STEP], &r);
    return v > 0x8000u ? 0x8000u : v;
}

/* headphone route (audio.h AUD_HPVOL_REG): hp_gain[n] = 32768 * 10^(-n / 20),
   n = 0..63 dB below AUD_HPVOL_REF_DB, the level iOS writes at full volume
   (+2 dB, QEMU run of re-log 57) = CSND unity, as amp step 19 on the speaker
   route: full volume is full scale on both outputs */
static const u16 hp_gain[64] = {
    32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362, 9235,
    8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677, 3277, 2920, 2603, 2320,
    2068, 1843, 1642, 1464, 1305, 1163, 1036, 924, 823, 734, 654, 583,
    519, 463, 413, 368, 328, 292, 260, 232, 207, 184, 164, 146,
    130, 116, 104, 92, 82, 73, 65, 58, 52, 46, 41, 37,
    33, 29, 26, 23,
};

static u32 hpvol_to_vol(u32 reg)
{
    int32_t db = (int32_t)(reg & 0x7F) - ((reg & 0x40) ? 0x80 : 0);
    int32_t att = (int32_t)AUD_HPVOL_REF_DB - db;
    if (reg & 0x80) return 0;                           /* mute */
    if (att <= 0) return 0x8000u;                       /* louder than iOS's full volume: clamp */
    return att > 63 ? 0 : hp_gain[att];
}

/* CSND's volume: the CS42L58's headphone volume while iOS is on its headphone
   route (the amp is shut down then), the amp's otherwise */
static u32 out_vol(void)
{
    if (A.hp_state == 1 && A.hp_reg_valid) return hpvol_to_vol(A.hp_reg);
    return A.amp_vol;
}

static void csnd_channel(u32 ch, u32 pa, u32 bytes, u32 vol)
{
    u32 b = CSND_CH(ch);
    csnd_w16(b + CSND_CH_CNT, 0);
    csnd_w16(b + CSND_CH_SR, 0x10000u - shim_audio_cfg.rate_div);
    csnd_w32(b + CSND_CH_VOL, vol);
    csnd_w32(b + CSND_CH_CAPVOL, vol);
    csnd_w32(b + CSND_CH_ADDR, pa);
    csnd_w32(b + CSND_CH_SIZE, bytes);
    csnd_w32(b + CSND_CH_LOOP, pa);
    csnd_w32(b + CSND_CH_ADPCM0, 0);
    csnd_w32(b + CSND_CH_ADPCM1, 0);
    csnd_w16(b + CSND_CH_CNT, CSND_CH_START | CSND_CH_FMT_PCM16 | CSND_CH_RPT_LOOP | CSND_CH_LERP);
}

static void csnd_start(void)
{
    u32 n = shim_audio_cfg.ring_frames, pa = shim_audio_cfg.pcm_pa;
    u8 *p = (u8 *)ring_l();
    if (!p) return;
    for (u32 i = 0; i < n * 4; i += 4) *(u32 *)(p + i) = 0;
    dclean(p, n * 4);
    A.wr = shim_audio_cfg.lead_frames & ring_mask();
    A.phase = 0;
    A.idle_words = 0;
    A.csnd_moved = 0;
    A.csnd_on = 1;
    shim_audio_stat.csnd_starts++;
    shim_audio_stat.state |= 4;
    aud_map_io();
    if (!csnd_usable()) return;
    i2s_out_enable();
    csnd_w16(CSND_MASTER_VOL, 0x8000);
    csnd_w16(CSND_CNT, CSND_CNT_EN | CSND_CNT_RS_FILTER);
    /* VOL: right in the low half, left in the high half (libn3ds csnd.h) */
    A.vol_applied = A.vol;
    csnd_channel(AUD_CH_L, pa, n * 2, A.vol_applied << 16);
    csnd_channel(AUD_CH_R, pa + n * 2, n * 2, A.vol_applied);
    /* unpause both back to back: they stay sample-aligned */
    {
        u32 l = csnd_r16(CSND_CH(AUD_CH_L) + CSND_CH_CNT) | CSND_CH_PLAYING;
        u32 r = csnd_r16(CSND_CH(AUD_CH_R) + CSND_CH_CNT) | CSND_CH_PLAYING;
        csnd_w16(CSND_CH(AUD_CH_L) + CSND_CH_CNT, l);
        csnd_w16(CSND_CH(AUD_CH_R) + CSND_CH_CNT, r);
    }
}

static void csnd_stop(void)
{
    A.csnd_on = 0;
    shim_audio_stat.state &= ~4u;
    if (!csnd_usable()) return;
    csnd_w16(CSND_CH(AUD_CH_L) + CSND_CH_CNT, 0);
    csnd_w16(CSND_CH(AUD_CH_R) + CSND_CH_CNT, 0);
}

/* Real hardware only: keep the write index LEAD frames ahead of where CSND
   actually plays (the position register counts through the ring). */
static void csnd_resync(void)
{
    u32 pa = shim_audio_cfg.pcm_pa, n = shim_audio_cfg.ring_frames, pos, lead;
    if (!A.csnd_on || !csnd_usable()) return;
    pos = csnd_r32(CSND_CH(AUD_CH_L) + CSND_CH_ADDR);
    if (pos < pa || pos >= pa + n * 2) return;      /* not a position in our ring */
    if (!A.csnd_moved) {                            /* an emulator without a CSND model reads back */
        if (pos == pa) return;                      /* the start address forever: never resync there */
        A.csnd_moved = 1;
    }
    lead = (A.wr - ((pos - pa) >> 1)) & ring_mask();
    if (lead < (n >> 4) || lead > (n >> 1)) {
        A.wr = (((pos - pa) >> 1) + shim_audio_cfg.lead_frames) & ring_mask();
        A.phase = 0;
        shim_audio_stat.csnd_resyncs++;
    }
}

/* ---- microphone -------------------------------------------------------------
   iOS's input stream is the I2S RX FIFO: 16-bit words, L then R, at the same
   word clock as the output. The samples come from the 3DS MIC block, which the
   shim drains itself on every service (mic_drain: 1 ms apart while the input
   stream runs, RX_STEP_TICKS; the FIFO holds 2.7 ms) into the ring aud_mic_t.
   mic_fill reads the ring at the mic's rate (11.9 kHz), interpolates linearly
   to the output frame rate, scales by AUD_MIC_GAIN_Q15 (iOS adds that much
   again) and sends the value to both channels. The reader trails the writer by
   MIC_LAG; an empty ring repeats the last value, a reader that fell too far
   behind jumps. The ARM9 powers the CODEC's ADC while aud_mic_t.want is set.
   Anything unexpected from the MIC block -> one log line, mic off, silence. */
#define MIC_LAG_DIV 50          /* trail by rate / 50 samples (20 ms) */

static volatile aud_mic_t *mic_ring(void) { return (volatile aud_mic_t *)p2v(AUD_MIC_PA, sizeof(aud_mic_t)); }

static void mic_publish(volatile aud_mic_t *m)     /* the ARM9 reads line 0 from RAM */
{
    dclean((const void *)m, 32);
}

static void mic_hw_stop(volatile aud_mic_t *m, u32 state)
{
    if (A.mic_on) mic_cnt_w(0);
    A.mic_on = 0;
    A.mic_synced = 0;
    m->state = state;
    mic_publish(m);
}

static void mic_fail(volatile aud_mic_t *m, u32 why, u32 cnt)
{
    static const char *const what[] = { "?", "no samples for 250 ms", "FIFO never empties",
                                        "MIC block switched itself off", "no IO window" };
    mic_hw_stop(m, AUD_MIC_FAILED);
    m->why = why;
    mic_publish(m);
    if (logging()) {
        log_str("ios3ds-audio: mic: ");
        log_str(what[why < 5 ? why : 0]);
        log_kv(" (MIC_CNT ", cnt);
        log_str("): input stays silent\n");
    }
}

static void mic_hw_start(volatile aud_mic_t *m, u32 now)
{
    aud_map_io();
    if (!A.io_ok) { mic_fail(m, 4, 0); return; }
    mic_cnt_w(0);
    mic_cnt_w(MIC_CNT_EN | MIC_CNT_CLEAR | MIC_CNT_DIV(AUD_MIC_DIV));
    A.mic_on = 1;
    A.mic_t_data = now;
    A.mic_synced = 0;
    m->rate = AUD_I2S2_HZ / (1 + AUD_MIC_DIV);
    m->state = AUD_MIC_RUNNING;
    m->starts++;
    mic_publish(m);
    if (logging()) {
        log_kv("ios3ds-audio: mic on, Hz=", m->rate);
        log_kv(" MIC_CNT=", mic_cnt_r());
        log_putc('\n');
    }
}

static void mic_drain(volatile aud_mic_t *m, u32 now)
{
    u32 n, wr = m->wr, cnt = 0;
    for (n = 0; n < AUD_MIC_WORDS_MAX; n++) {
        u32 v;
        cnt = mic_cnt_r();
        if (cnt & MIC_CNT_EMPTY) break;
        if (!(cnt & MIC_CNT_EN)) { mic_fail(m, 3, cnt); return; }
        if (cnt & MIC_CNT_OVERRUN) {
            m->overruns++;
            mic_cnt_w(cnt & ~MIC_CNT_OVERRUN);
        }
        v = mic_data_r();
        m->s[wr & (AUD_MIC_SAMPLES - 1)] = (int16_t)v;
        m->s[(wr + 1) & (AUD_MIC_SAMPLES - 1)] = (int16_t)(v >> 16);
        wr += 2;
    }
    m->wr = wr;
    if (n == AUD_MIC_WORDS_MAX) { mic_fail(m, 2, cnt); return; }
    if (n) A.mic_t_data = now;
    else if (now - A.mic_t_data > AUD_MIC_NODATA_TICKS) mic_fail(m, 1, cnt);
}

/* the reader: where in the ring the next output frame comes from */
typedef struct { u32 rd, frac, phase; int32_t last; } mic_cur_t;

/* one RX word from cursor c (wr = the ring's write count); returns 0 when the
   ring ran dry (the value repeats). `real` = count statistics. */
static int mic_word(volatile aud_mic_t *m, u32 wr, mic_cur_t *c, int16_t *out, int real)
{
    int ok = 1;
    if (!c->phase) {                            /* a new frame: between mic samples rd and rd + 1 */
        if ((int)(wr - c->rd) >= 2) {
            int32_t s0 = m->s[c->rd & (AUD_MIC_SAMPLES - 1)];
            int32_t s1 = m->s[(c->rd + 1) & (AUD_MIC_SAMPLES - 1)];
            int32_t v = s0 + (((s1 - s0) * (int32_t)(c->frac >> 2)) >> 14);
            c->last = (v * (int32_t)AUD_MIC_GAIN_Q15 + (1 << 14)) >> 15;
            c->frac += A.mic_step;
            while (c->frac >= 0x10000u && (int)(wr - c->rd) > 1) {
                c->frac -= 0x10000u;
                c->rd++;
                if (real) shim_audio_stat.mic_samples++;
            }
        } else {
            ok = 0;
            if (real) shim_audio_stat.mic_underruns++;
        }
        if (real) {
            int32_t dd = c->last - A.dbg_prev;
            if (dd < 0) dd = -dd;
            if (dd > 16) A.dbg_jumps++;
            if ((u32)dd > A.dbg_maxd) A.dbg_maxd = (u32)dd;
            A.dbg_prev = c->last;
        }
    }
    *out = (int16_t)c->last;
    c->phase ^= 1;
    return ok;
}

static void mic_cur_get(mic_cur_t *c) { c->rd = A.mic_rd; c->frac = A.mic_frac; c->phase = A.rx_phase; c->last = A.mic_last; }
static void mic_cur_put(const mic_cur_t *c) { A.mic_rd = c->rd; A.mic_frac = c->frac; A.rx_phase = c->phase; A.mic_last = c->last; }

static void mic_fill(u8 *dst, u32 words, u32 sw)
{
    volatile aud_mic_t *m = mic_ring();
    int16_t *out = (int16_t *)dst;
    mic_cur_t c;
    u32 wr, r, i;
    if (!m || sw != 1 || !A.mic_on || m->state != AUD_MIC_RUNNING) goto silence;
    wr = m->wr;
    if (!A.mic_synced || (int)(wr - A.mic_rd) < 0 || wr - A.mic_rd > AUD_MIC_SAMPLES / 2) {
        if (A.mic_synced) shim_audio_stat.mic_resyncs++;
        A.mic_rd = wr - udivmod(m->rate, MIC_LAG_DIV, &r);
        A.mic_frac = 0;
        A.mic_step = udivmod(m->rate << 16, udivmod(CSND_CLOCK, shim_audio_cfg.rate_div, &r), &r);
        A.mic_synced = 1;
    }
    mic_cur_get(&c);
    /* frames are 4 bytes (L, R): the word's place in its frame comes from the
       address, not from what the previous stream left behind */
    c.phase = ((u32)(uintptr_t)dst >> 1) & 1;
    for (i = 0; i < words; i++) mic_word(m, wr, &c, &out[i], 1);
    mic_cur_put(&c);
    return;
silence:
    A.mic_synced = 0;
    for (i = 0; i < (words << sw); i++) dst[i] = 0;
}

/* iOS's input stream started / stopped (debounced by the caller) */
static void mic_want(u32 on, u32 now)
{
    volatile aud_mic_t *m = mic_ring();
    if (!m) return;
    if (m->magic != AUD_MIC_MAGIC) {            /* first use: our lines are ours from now on */
        m->magic = AUD_MIC_MAGIC;
        m->state = AUD_MIC_OFF;
        m->wr = m->overruns = m->starts = m->want = m->why = 0;
        mic_publish(m);
    }
    if (on == A.mic_want) return;
    A.mic_want = on;
    m->want = on;
    if (on) {
        dinval(&m->arm9_magic, 32);             /* the ARM9's line */
        if (m->arm9_magic == AUD_MIC_MAGIC && m->disabled) {
            m->state = AUD_MIC_DISABLED;
            mic_publish(m);
        } else if (m->state != AUD_MIC_FAILED) {
            mic_hw_start(m, now);
        } else {
            mic_publish(m);
        }
    } else {
        mic_hw_stop(m, m->state == AUD_MIC_RUNNING ? AUD_MIC_OFF : m->state);
    }
}

/* ---- one PL080 channel ---------------------------------------------------- */
/* Transfer up to `budget` items of the channel's programmed width; walk the
   LLI chain like the controller does. Returns the number transferred. */
static u32 run_chan(volatile u32 *r, u32 ch, u32 budget, int mode, u32 *tc)
{
    u32 done = 0;
    for (int guard = 0; guard < LLI_GUARD; guard++) {
        u32 ctl = r[CH_CTL], size = ctl & CTL_SIZE_MASK, sw = CTL_SWIDTH(ctl), lli, *q;
        if (size) {
            u32 n = budget - done;
            if (n > size) n = size;
            if (!n) break;
            if (mode == MODE_TX) {
                u32 src = r[CH_SRC];
                const u16 *p = (sw == 1 && !(src & 1)) ? (const u16 *)p2v(src, n * 2) : NULL;
                if (!p) shim_audio_stat.bad_addr++;
                sink_put(p, n);
                shim_audio_stat.tx_words += n;
                shim_audio_stat.last_src = src;
            } else if (mode == MODE_RX) {
                u32 dst = r[CH_DST];
                u8 *p = (u8 *)p2v(dst, n << sw);
                if (p) {
                    mic_fill(p, n, sw);
                    dclean(p, n << sw);         /* iOS reads it through other mappings */
                } else {
                    shim_audio_stat.bad_addr++;
                }
                shim_audio_stat.rx_words += n;
            }
            if (ctl & CTL_SI) r[CH_SRC] += n << sw;
            if (ctl & CTL_DI) r[CH_DST] += n << sw;
            size -= n;
            done += n;
            r[CH_CTL] = (ctl & ~CTL_SIZE_MASK) | size;
            if (size) break;                /* out of budget inside this item */
        }
        /* item complete */
        if ((ctl & CTL_I) && (r[CH_CFG] & CFG_ITC)) *tc |= 1u << ch;
        lli = r[CH_LLI] & ~3u;
        q = lli ? (u32 *)p2v(lli, 16) : NULL;
        if (!q) {                           /* end of chain: the channel disables itself */
            if (lli) shim_audio_stat.bad_addr++;
            r[CH_CFG] &= ~CFG_E;
            break;
        }
        r[CH_SRC] = q[0];
        r[CH_DST] = q[1];
        r[CH_LLI] = q[2];
        r[CH_CTL] = q[3];
    }
    return done;
}

/* ticks until the item a paced channel is working on ends */
static u32 ticks_to_item_end(volatile u32 *r)
{
    u32 size = r[CH_CTL] & CTL_SIZE_MASK, rem, t;
    u32 need = size * shim_audio_cfg.rate_div;      /* size <= 4095, div <= 65535 */
    need = need > A.pace_frac ? need - A.pace_frac : 0;
    t = udivmod(need + 127, 128, &rem);
    return t ? t : 1;
}

/* ---- headphones ----------------------------------------------------------
   AppleEmbeddedAudio learns about the jack through audio0's
   function-hp_detect (a pin on the fake GPIO page) and re-reads it on audio0's
   interrupt (VIC0 source 12), both set up in dt_audio.py. The ARM9 polls the
   3DS jack (GPIO2 bit 0, debounced) and publishes it in aud_mic_t.jack; the
   shim mirrors it into the pin and raises the interrupt on every change. iOS
   then switches its output route (and its volume memory) between speaker and
   headphones; the 3DS CODEC switches the analogue output itself (the ARM9's
   jack polling). boot.cfg hp_route=0 keeps iOS on the speaker route. */
static void hp_service(void)
{
    volatile aud_mic_t *m = mic_ring();
    volatile u32 *pin;
    u32 want;
    if (!shim_audio_cfg.gpio_pa || !m) return;
    pin = hp_pin_word();
    if (!A.hp_init) {                           /* before AppleEmbeddedAudio starts: no headphones */
        A.hp_init = 1;
        A.hp_state = 0;
        *pin &= ~AUD_HP_GPIO_BIT;
        shim_audio_stat.hp_state = 0;
    }
    dinval(&m->arm9_magic, 32);                 /* the ARM9's line */
    if (m->arm9_magic != AUD_MIC_MAGIC) return;
    want = m->hp_disabled ? AUD_HP_OFF : (m->jack ? 1 : 0);
    if (want == A.hp_state) return;
    if (want == AUD_HP_OFF && !A.hp_state) {    /* switched off while out: nothing to tell iOS */
        A.hp_state = AUD_HP_OFF;
        shim_audio_stat.hp_state = AUD_HP_OFF;
        return;
    }
    if (A.hp_state == AUD_HP_OFF) return;       /* off stays off */
    if (want == 1) *pin |= AUD_HP_GPIO_BIT;
    else *pin &= ~AUD_HP_GPIO_BIT;
    A.hp_state = want;
    shim_audio_stat.hp_state = want;
    shim_audio_stat.hp_changes++;
    A.vol = out_vol();
    shim_audio_stat.volume = A.vol;
    A.hp_inject = hp_vector() != 0;             /* no vector yet: the driver reads the pin when it starts */
    if (logging()) {
        log_str(want == 1 ? "ios3ds-audio: headphones in" : "ios3ds-audio: headphones out");
        log_str(A.hp_inject ? ", jack interrupt\n" : " (no jack interrupt handler yet)\n");
    }
}

/* ---- the emulator: advance everything to tick `now` ----------------------- */
u32 shim_cam_irq(u32 now);

void aud_service(u32 now)
{
    volatile u32 *d = dmac_regs();
    u32 words = 0, tc = 0, tx_seen = 0, rx_seen = 0, tx_done = 0, enabled = 0, next = 0, have_next = 0;
    u32 wr0, v;

    shim_audio_stat.services++;
    hp_service();

    /* write-1-to-clear registers: the page is plain RAM, so apply the writes now */
    v = d[DMAC_INT_TC_CLEAR];
    if (v) {
        d[DMAC_INT_TC_STATUS] &= ~v;
        d[DMAC_RAW_TC] &= ~v;
        d[DMAC_INT_TC_CLEAR] = 0;
    }
    v = d[DMAC_INT_ERR_CLEAR];
    if (v) {
        d[DMAC_INT_ERR_STATUS] &= ~v;
        d[DMAC_RAW_ERR] &= ~v;
        d[DMAC_INT_ERR_CLEAR] = 0;
    }

    /* the I2S word clock */
    if (A.pacing) {
        u32 dt = now - A.pace_last;
        if (dt > MAX_DT) dt = MAX_DT;
        words = udivmod(dt * 128u + A.pace_frac, shim_audio_cfg.rate_div, &A.pace_frac);
    } else {
        A.pace_frac = 0;
    }
    A.pace_last = now;
    wr0 = A.wr;

    if (A.mic_on) {
        volatile aud_mic_t *m = mic_ring();
        if (m) mic_drain(m, now);
    }

    for (u32 ch = 0; ch < 8; ch++) {
        volatile u32 *r = d + DMAC_CH(ch);
        u32 cfg = r[CH_CFG], flow = CFG_FLOW(cfg);
        if (!(cfg & CFG_E) || (cfg & CFG_HALT)) continue;
        if (flow == 1 && IS_I2S(r[CH_DST])) {
            if (!tx_seen) {
                tx_seen = 1;
                if (!A.tx_on) {             /* stream start */
                    shim_audio_stat.starts++;
                    shim_audio_stat.peak = 0;
                    if (!A.csnd_on) csnd_start();
                    wr0 = A.wr;
                    if (logging()) {
                        log_kv("ios3ds-audio: tx start ch", ch);
                        log_kv(" src=", r[CH_SRC]);
                        log_kv(" lli=", r[CH_LLI]);
                        log_kv(" ctl=", r[CH_CTL]);
                        log_kv(" cfg=", cfg);
                        log_putc('\n');
                    }
                }
                tx_done = run_chan(r, ch, words, MODE_TX, &tc);
            }
        } else if (flow == 2 && IS_I2S(r[CH_SRC])) {
            if (!rx_seen) {
                rx_seen = 1;
                if (!A.rx_on && logging()) {        /* input stream start */
                    log_kv("ios3ds-audio: rx start ch", ch);
                    log_kv(" dst=", r[CH_DST]);
                    log_kv(" lli=", r[CH_LLI]);
                    log_kv(" ctl=", r[CH_CTL]);
                    log_kv(" cfg=", cfg);
                    log_putc('\n');
                }
                run_chan(r, ch, words, MODE_RX, &tc);
            }
        } else if (flow == 1) {
            run_chan(r, ch, ~0u, MODE_DROP, &tc);   /* e.g. a UART transmit: gone at once */
            continue;
        } else {
            continue;                               /* peripheral -> memory with no peripheral: waits forever */
        }
        if (r[CH_CFG] & CFG_E) {
            u32 t = ticks_to_item_end(r);
            /* input: iOS reads the ring up to where the sample clock says the
               DMA is, which can be ahead of our last service; come back soon
               so the newest input is never more than RX_STEP_TICKS old */
            if (flow == 2 && t > RX_STEP_TICKS) t = RX_STEP_TICKS;
            t += now;
            if (!have_next || (int)(t - next) < 0) next = t;
            have_next = 1;
        }
    }
    /* the MIC FIFO lasts 2.7 ms: drain it every RX_STEP_TICKS while the mic is
       on, also when the input channel is idle between two queued commands */
    if (A.mic_on) {
        u32 t = now + RX_STEP_TICKS;
        if (!have_next || (int)(t - next) < 0) next = t;
        have_next = 1;
    }
#ifndef AUD_HOST_TEST
    {   /* the camera FIFO drain rides on the same wake-ups (kernel/shim/cam.c) */
        u32 t = shim_cam_irq(now);
        if (t) {
            if (!have_next || (int)(t - next) < 0) next = t;
            have_next = 1;
        }
    }
#endif
    for (u32 ch = 0; ch < 8; ch++)
        if (d[DMAC_CH(ch) + CH_CFG] & CFG_E) enabled |= 1u << ch;
    d[DMAC_ENBLD_CHNS] = enabled;

    if (A.tx_on && !tx_seen) {
        shim_audio_stat.stops++;
        if (logging()) {
            log_kv("ios3ds-audio: tx stop words=", shim_audio_stat.tx_words);
            log_kv(" peak=", shim_audio_stat.peak);
            log_kv(" tc=", shim_audio_stat.tc_irqs);
            log_putc('\n');
        }
    }
    if (A.rx_on && !rx_seen && logging()) {
        log_kv("ios3ds-audio: rx stop words=", shim_audio_stat.rx_words);
        log_kv(" mic jumps>16=", A.dbg_jumps);
        log_kv(" maxd=", A.dbg_maxd);
        log_kv(" underruns=", shim_audio_stat.mic_underruns);
        log_kv(" resyncs=", shim_audio_stat.mic_resyncs);
        A.dbg_jumps = A.dbg_maxd = 0;
        log_putc('\n');
    }
    A.tx_on = tx_seen;
    A.rx_on = rx_seen;
    /* between two queued input commands the channel may be idle for a moment:
       the mic stays on until the input stream has been gone for a while */
    if (rx_seen) A.rx_last = now;
    mic_want(rx_seen || (A.mic_want && now - A.rx_last < AUD_MIC_HOLD_TICKS), now);

    /* the sink gets exactly `words` words per step: what iOS played, then silence */
    if (A.csnd_on) {
        if (words > tx_done) sink_put(NULL, words - tx_done);
        sink_clean(wr0, A.wr);
        if (tx_seen) {
            A.idle_words = 0;
        } else {
            A.idle_words += words;
            if (A.idle_words > IDLE_STOP_WORDS) csnd_stop();
        }
        csnd_resync();
        if (A.vol != A.vol_applied && csnd_usable()) {
            A.vol_applied = A.vol;
            csnd_w32(CSND_CH(AUD_CH_L) + CSND_CH_VOL, A.vol_applied << 16);
            csnd_w32(CSND_CH(AUD_CH_R) + CSND_CH_VOL, A.vol_applied);
        }
    }
    A.pacing = tx_seen || rx_seen || A.csnd_on;
    A.next = next;
    A.next_valid = have_next;

    if (tc) {
        d[DMAC_INT_TC_STATUS] |= tc;
        d[DMAC_RAW_TC] |= tc;
        shim_audio_stat.tc_irqs++;
        A.inject = 1;
    }
    d[DMAC_INT_STATUS] = d[DMAC_INT_TC_STATUS] | d[DMAC_INT_ERR_STATUS];

    v = (shim_audio_stat.state & ~3u) | (tx_seen ? 1u : 0) | (rx_seen ? 2u : 0);
    if (tc || v != shim_audio_stat.state || !(shim_audio_stat.services & 63)) {
        shim_audio_stat.state = v;
        dclean((const void *)&shim_audio_stat, sizeof(shim_audio_stat));    /* the ARM9 reads RAM, not our cache */
    }
}

/* ---- I2C ------------------------------------------------------------------ */
/* Replaces AppleARMIICDevice's transfer method (0x802e4d1e, vtable slot
   0x35c): (this, op: 1 = read 2 = write, sub-address length, sub-address,
   data length, data). Every device gets a 256-register file. */
#define IIC_DEVS 4
static struct { void *nub; u8 reg[256]; } iic_dev[IIC_DEVS] AUD_DATA = { { 0 } };
static u32 iic_logged AUD_DATA = 0;

#ifndef AUD_HOST_TEST
int shim_mcu_iic(void *nub, u32 op, u32 sublen, const u8 *sub, u32 len, u8 *buf);
#else
static int shim_mcu_iic(void *nub, u32 op, u32 sublen, const u8 *sub, u32 len, u8 *buf)
{ (void)nub; (void)op; (void)sublen; (void)sub; (void)len; (void)buf; return -1; }
#endif

int shim_iic_transfer(void *nub, u32 op, u32 sublen, const u8 *sub, u32 len, u8 *buf)
{
    u8 *regs = NULL;
    u32 a = 0, changed = 0;
    if ((sublen && !sub) || !len || !buf) return (int)0xE00002C2;     /* kIOReturnBadArgument, like the original */
    {   /* the accelerometer (LIS302DL) is answered from the MCU: tools/ios3ds-mcu/shim/mcu.c */
        int r = shim_mcu_iic(nub, op, sublen, sub, len, buf);
        if (r != -1) return r;
    }
    for (int i = 0; i < IIC_DEVS && !regs; i++) {
        if (iic_dev[i].nub == nub || !iic_dev[i].nub) {
            iic_dev[i].nub = nub;
            regs = iic_dev[i].reg;
        }
    }
    if (!regs) regs = iic_dev[IIC_DEVS - 1].reg;
    if (sublen) a = sub[sublen - 1];
    a &= 0x7F;                                  /* CS42L58: bit 7 of the MAP byte = auto-increment */
    if (op == 2 && !sublen && len == 1 && ((buf[0] & 7) == 3 || buf[0] == 4)) {   /* LM48821 (see amp_gain) */
        A.amp_vol = buf[0] == 4 ? 0 : amp_step_to_vol(buf[0] >> 3);
        A.vol = out_vol();
        shim_audio_stat.amp_step = buf[0] == 4 ? AUD_AMP_OFF : buf[0] >> 3u;
        shim_audio_stat.volume = A.vol;
        if (logging() && (u32)buf[0] != regs[0]) {
            if (buf[0] == 4) {
                log_kv("ios3ds-audio: amp off -> CSND volume ", A.vol);
                log_putc('\n');
            } else {
                log_kv("ios3ds-audio: amp step ", buf[0] >> 3);
                log_kv(" -> CSND volume ", A.vol);
                log_putc('\n');
            }
        }
    }
    if (op == 2 && sublen && a <= AUD_HPVOL_REG && AUD_HPVOL_REG < a + len) {    /* CS42L58 headphone volume */
        A.hp_reg = buf[AUD_HPVOL_REG - a];
        A.hp_reg_valid = 1;
        A.vol = out_vol();
        shim_audio_stat.hp_vol = A.hp_reg;
        shim_audio_stat.volume = A.vol;
    }
    if (op == 2) {
        for (u32 i = 0; i < len; i++) {
            if (regs[(a + i) & 0xFF] != buf[i]) changed = 1;
            regs[(a + i) & 0xFF] = buf[i];
        }
        shim_audio_stat.iic_writes++;
    } else {
        for (u32 i = 0; i < len; i++) buf[i] = regs[(a + i) & 0xFF];
        shim_audio_stat.iic_reads++;
    }
    /* the first 300 accesses (driver start-up), then only writes that change a register */
    if (logging() && (iic_logged < 300 || (changed && iic_logged < 3000))) {
        iic_logged++;
        log_kv(op == 2 ? "ios3ds-audio: iic w dev=" : "ios3ds-audio: iic r dev=", (u32)(uintptr_t)nub & 0xFFFF);
        log_kv(" reg=", a);
        log_kv(" len=", len);
        log_kv(" val=", buf[0]);
        log_putc('\n');
    }
    return 0;
}

/* ======================================================================== */
#ifndef AUD_HOST_TEST

static void pt_load(u32 ticks)
{
    REG32(PRIV_VA + PT_CTRL) = 0;
    REG32(PRIV_VA + PT_ISR) = 1;
    REG32(PRIV_VA + PT_LOAD) = (ticks ? ticks : 1) << TB_SHIFT;
    REG32(PRIV_VA + PT_CTRL) = PT_CTRL_RUN;
}

/* Called by shim_set_decrementer with the tick count XNU wants to sleep.
   Returns the count to program: shorter when the emulated DMA has an item
   ending first (the item's last sample is when the driver expects its IRQ). */
u32 shim_audio_setdec(u32 ticks)
{
    u32 now;
    if (!shim_audio_cfg.dmac_pa) return ticks;
    now = now_ticks();
    if (!ticks) ticks = 1;
    A.k_deadline = now + (ticks > 0x40000000u ? 0x40000000u : ticks);
    A.k_valid = 1;
    A.clamped = 0;
    if (A.next_valid) {
        int rem = (int)(A.next - now);
        if (rem < 1) rem = 1;
        if ((u32)rem < ticks) {
            ticks = (u32)rem;
            A.clamped = 1;
        }
    }
    return ticks;
}

/* Called from shim_irq (through audio_glue.S) for every interrupt, in IRQ
   mode, GIC already acknowledged (iar). Returns what the glue does next:
     0  carry on in shim_irq (timer -> XNU's FIQ handler, VBlank -> CLCD, ...)
     1  the interrupt was only ours: return to the interrupted code
     2  enter XNU's IRQ path: the DMA controller's interrupt is pending in the fake VIC */
u32 shim_audio_irq(u32 iar)
{
    u32 id = iar & 0x3FF, now;
    int early;
    if (!shim_audio_cfg.dmac_pa || (id != 29 && id != 42)) return 0;
    now = now_ticks();
    aud_service(now);

    if (id == 42) {
        /* VBlank: shim_irq may inject the CLCD interrupt now; ours follows
           through the private timer a moment later */
        if (A.inject || A.hp_inject) {
            REG32(PRIV_VA + PT_CTRL) = 0;
            REG32(PRIV_VA + PT_LOAD) = 2;
            REG32(PRIV_VA + PT_CTRL) = PT_CTRL_RUN;
            A.clamped = 1;
        }
        return 0;
    }

    early = A.clamped && A.k_valid && (int)(A.k_deadline - now) > 0;
    if (!early && !A.inject && !A.hp_inject) {
        A.clamped = 0;
        return 0;                   /* XNU's deadline: its FIQ handler takes it */
    }
    REG32(PRIV_VA + PT_ISR) = 1;
    REG32(PRIV_VA + GICC_EOIR) = iar;
    if (early) {
        u32 t = A.k_deadline - now;
        shim_audio_stat.early_wakes++;
        A.clamped = 0;
        if (A.next_valid) {
            int rem = (int)(A.next - now);
            if (rem < 1) rem = 1;
            if ((u32)rem < t) {
                t = (u32)rem;
                A.clamped = 1;
            }
        }
        pt_load(t);
    } else {
        pt_load(2);                 /* XNU's deadline is due too: deliver the DMA interrupt, then come back */
    }
    if (A.inject) {
        /* like the CLCD frame interrupt in shim_irq: "source 16 pending" */
        u32 vic = REG32(SHIM_VA + SHIM_CFG_VIC0) - AXI_PA + AXI_VA;
        REG32(vic + 0xF00) = REG32(vic + 0x100 + 16 * 4);
        A.inject = 0;
        shim_audio_stat.injected++;
        return 2;
    }
    if (A.hp_inject) {
        /* the jack interrupt, after any DMA interrupt (one vector at a time) */
        u32 vic = REG32(SHIM_VA + SHIM_CFG_VIC0) - AXI_PA + AXI_VA;
        u32 v = REG32(vic + 0x100 + AUD_HP_VIC_SRC * 4);
        A.hp_inject = 0;
        if (v) {
            REG32(vic + 0xF00) = v;
            return 2;
        }
    }
    return 1;
}

#else   /* host tests reach the internals through these */
u32 aud_test_inject(void) { u32 v = A.inject; A.inject = 0; return v; }
u32 aud_test_hp_inject(void) { u32 v = A.hp_inject; A.hp_inject = 0; return v; }
u32 aud_test_next(u32 *valid) { *valid = A.next_valid; return A.next; }
u32 aud_test_wr(void) { return A.wr; }
void aud_test_reset(void)
{
    u8 *p = (u8 *)&A;
    for (u32 i = 0; i < sizeof(A); i++) p[i] = 0;
    p = (u8 *)&shim_audio_stat;
    for (u32 i = 0; i < sizeof(shim_audio_stat); i++) p[i] = 0;
    p = (u8 *)iic_dev;
    for (u32 i = 0; i < sizeof(iic_dev); i++) p[i] = 0;
    A.vol = A.amp_vol = 0x8000;
    shim_audio_stat.hp_vol = AUD_HPVOL_NONE;
    iic_logged = 0;
}
#endif
