/*
 * ios3ds ARM9: 3DS touch panel (CODEC "TSC" on SPI bus 2) + HID pad -> input
 * mailbox in the XNU shim region. NOT wired into boot/Makefile yet; to wire:
 * add this file to ARM9_OBJ, call input_poll(cfg.shim_pa) from the monitor
 * loop in boot/arm9/main.c (e.g. `if ((tick & 7) == 0)`), see
 * docs/input-design.md.
 *
 * Hardware facts (all registers live in the 0x101xxxxx "common" IO block that
 * both CPUs can reach, like HID_PAD at 0x10146000 which this monitor already
 * polls, so no ARM11 help is needed):
 *   CFG11_SPI_CNT 0x101401C0   bit n = bus n+1 uses the new "NSPI" interface
 *                              (libn3ds include/arm11/drivers/cfg11.h, spi.c NSPI_init)
 *   NSPI bus 2    0x10142800   CS0 = CTR CODEC, 16 MHz
 *                              (libn3ds spi.h NSPI2_REGS_BASE, spi.c g_spiDevTable[3];
 *                               linux-3ds nintendo3ds.dtsi spi@10142800 / tsc@0)
 *   CODEC framing              [page select: reg 0 <- page], then
 *                              byte0 = reg << 1 | read, data follows
 *                              (libn3ds codec.c switchPage/readRegArray/writeReg)
 *   touch ADC setup            page 0x67 regs 0x17-0x27, the sequence GodMode9
 *                              (arm11/source/hw/codec.c CODEC_Init) and linux-3ds
 *                              (drivers/platform/nintendo3ds/tsc/touch.c
 *                              touch_initialize + touch_enable) use after a plain
 *                              FIRM boot; libn3ds codec.c touchAndCirclePadInit is
 *                              the same sequence with HWCAL values.
 *   ADC data                   page 0xFB reg 1, 0x34 bytes, big-endian u16:
 *                              touchX[5] touchY[5] cpadY[8] cpadX[8]
 *                              (libn3ds codec.h CdcAdcData, CODEC_getRawAdcData);
 *                              touchX bit 12 set = pen up; page 0x67 reg 0x26
 *                              bit 1 set = no new data.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tsc.h"
#include "mcu.h"

typedef struct {
    vu32 cnt;        /* 0x00 */
    vu32 cs;         /* 0x04 */
    vu32 blklen;     /* 0x08 */
    vu32 fifo;       /* 0x0C */
    vu32 fifo_stat;  /* 0x10 */
    vu32 autopoll;   /* 0x14 */
    vu32 int_mask;   /* 0x18 */
    vu32 int_stat;   /* 0x1C */
} nspi_t;

#define CFG11_SPI_CNT   (*(vu16 *)0x101401C0u)
#define NSPI2           ((nspi_t *)0x10142800u)
#define NSPI_EN         (1u << 15)
#define NSPI_DIR_S      (1u << 13)
#define NSPI_CLK_16MHZ  5u
#define NSPI_CS0        (0u << 6)
#define NSPI_FIFO_BUSY  1u
#define CODEC_CNT       (NSPI_EN | NSPI_CS0 | NSPI_CLK_16MHZ)

#define SPI_SPIN        200000u     /* bounded waits: the monitor must never hang */

#define PAGE_TOUCH      0x67u
#define PAGE_ADC        0xFBu

static bool spi_dead;
static u8 cur_page = 0xFF;

static void spi_wait(void)
{
    for (u32 i = 0; (NSPI2->cnt & NSPI_EN); i++)
        if (i > SPI_SPIN) { spi_dead = true; NSPI2->cnt = 0; return; }
}

static void fifo_wait(void)
{
    for (u32 i = 0; (NSPI2->fifo_stat & NSPI_FIFO_BUSY); i++)
        if (i > SPI_SPIN) { spi_dead = true; return; }
}

/* tx then rx in one chip-select window; buffers are u32-aligned, sizes in bytes */
static void spi_xfer(const u32 *tx, u32 txlen, u32 *rx, u32 rxlen)
{
    nspi_t *b = NSPI2;
    if (spi_dead) return;
    if (txlen) {
        b->blklen = txlen;
        b->cnt = CODEC_CNT | NSPI_DIR_S;
        for (u32 n = 0; n < txlen; n += 4) {
            if ((n & 31) == 0) fifo_wait();
            b->fifo = *tx++;
        }
        spi_wait();
    }
    if (rxlen) {
        b->blklen = rxlen;
        b->cnt = CODEC_CNT;
        for (u32 n = 0; n < rxlen; n += 4) {
            if ((n & 31) == 0) fifo_wait();
            *rx++ = b->fifo;
        }
        spi_wait();
    }
    b->cs = 0;                      /* deassert chip select */
}

static void cdc_page(u8 page)
{
    if (cur_page == page) return;
    cur_page = page;
    u32 w = (u32)page << 8;         /* bytes: reg 0 (write), page */
    spi_xfer(&w, 2, NULL, 0);
}

void cdc_write(u8 page, u8 reg, u8 val)
{
    cdc_page(page);
    u32 w = (u32)(reg << 1) | (u32)val << 8;
    spi_xfer(&w, 2, NULL, 0);
}

void cdc_read(u8 page, u8 reg, u32 *out, u32 len)
{
    cdc_page(page);
    u32 w = (u32)(reg << 1) | 1u;
    spi_xfer(&w, 1, out, len);
}

u8 cdc_read8(u8 page, u8 reg)
{
    u32 v = 0;
    cdc_read(page, reg, &v, 1);
    return (u8)v;
}

void cdc_mask(u8 page, u8 reg, u8 val, u8 mask)
{
    cdc_write(page, reg, (cdc_read8(page, reg) & ~mask) | (val & mask));
}

/* also used by boot/arm9/audio.c (CODEC sound setup, runs before tsc_init) */
void cdc_bus_init(void)
{
    /* all three buses on the new interface (what libn3ds and Horizon use) */
    CFG11_SPI_CNT = 7;
    NSPI2->cnt = 0;
    NSPI2->int_mask = 1;            /* no transfer-end IRQ */
    NSPI2->int_stat = 7;            /* ack anything pending */
    spi_dead = false;
    cur_page = 0xFF;
}

bool cdc_bus_dead(void) { return spi_dead; }
void cdc_forget_page(void) { cur_page = 0xFF; }    /* after a CODEC soft reset */

bool tsc_init(void)
{
    cdc_bus_init();

    /* touch + circle-pad ADC. Values 0x43/0x69/0x80/0x11 are libn3ds'
       launch-day HWCAL defaults (analogPrecharge 4, analogSense 3,
       analogXpPullup 6, analogStabilize 9, ymDriver 1, analogDebounce 0,
       analogInterval 1). */
    cdc_write(PAGE_TOUCH, 0x24, 0x98);  /* stop conversion */
    cdc_write(PAGE_TOUCH, 0x26, 0x00);
    cdc_write(PAGE_TOUCH, 0x25, 0x43);
    cdc_write(PAGE_TOUCH, 0x24, 0x18);
    cdc_write(PAGE_TOUCH, 0x17, 0x43);
    cdc_write(PAGE_TOUCH, 0x19, 0x69);
    cdc_write(PAGE_TOUCH, 0x1B, 0x80);
    cdc_write(PAGE_TOUCH, 0x27, 0x11);
    cdc_write(PAGE_TOUCH, 0x26, 0xEC);
    cdc_write(PAGE_TOUCH, 0x24, 0x18);
    cdc_write(PAGE_TOUCH, 0x25, 0x53);
    /* enable the touchscreen (libn3ds enableTouchscreen) */
    cdc_mask(PAGE_TOUCH, 0x26, 0x80, 0x80);
    cdc_mask(PAGE_TOUCH, 0x24, 0x00, 0x80);
    cdc_mask(PAGE_TOUCH, 0x25, 0x10, 0x3C);

    /* Probe: a stubbed/absent SPI block reads back 0 (or 0xFF with no MISO
       drive); a live CODEC returns what we just programmed. */
    u8 r25 = cdc_read8(PAGE_TOUCH, 0x25), r26 = cdc_read8(PAGE_TOUCH, 0x26);
    if (spi_dead) return false;
    return (r26 & 0x80) && (r25 & 0x3C) == 0x10 && r25 != 0xFF;
}

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

bool tsc_read(bool *pen, u16 *tx, u16 *ty, int16_t *cx, int16_t *cy)
{
    u32 raw32[0x34 / 4];
    const u8 *raw = (const u8 *)raw32;

    if (cdc_read8(PAGE_TOUCH, 0x26) & 2) return false;      /* no new data */
    cdc_read(PAGE_ADC, 0x01, raw32, 0x34);
    if (spi_dead) return false;

    /* pen is down only if all five conversions agree; position = their mean */
    u32 sx = 0, sy = 0;
    bool down = true;
    for (int i = 0; i < 5; i++) {
        u16 x = be16(raw + i * 2), y = be16(raw + 10 + i * 2);
        if (x & 0x1000) down = false;
        sx += x & 0xFFF;
        sy += y & 0xFFF;
    }
    *pen = down;
    *tx = (u16)(sx / 5);
    *ty = (u16)(sy / 5);
    *cy = (int16_t)((be16(raw + 0x14) & 0xFFF) - 2048);
    *cx = (int16_t)-((be16(raw + 0x24) & 0xFFF) - 2048);    /* X axis is inverted */
    return true;
}

/* ---- mailbox producer ---------------------------------------------------- */

#define MOVE_THRESH 12      /* raw ADC units (~1 px) before a move is pushed */

static u32 absdiff(u32 a, u32 b) { return a > b ? a - b : b - a; }

void input_poll(u32 shim_pa)
{
    static int state;               /* 0 = not initialised, 1 = running */
    static insample_t last;
    static u32 tick;
    volatile inmbox_t *m = (volatile inmbox_t *)(shim_pa + SHIM_INPUT_OFF);

    if (!shim_pa) return;
    if (!state) {
        for (u32 i = 0; i < sizeof(*m) / 4; i++) ((vu32 *)m)[i] = 0;
        m->version = INPUT_VERSION;
        m->tsc_ok = tsc_init();
        last.buttons = 0xFFFF;      /* force a first sample */
        m->magic = INPUT_MAGIC;
        state = 1;
    }

    insample_t s = last;
    s.tick = ++tick;
    s.buttons = (u16)(~HID_PAD & INBTN_PAD_MASK);
    /* SELECT+START stays the monitor's panic combo: do not forward it */
    if ((s.buttons & (HID_SELECT | HID_START)) == (HID_SELECT | HID_START))
        s.buttons &= (u16)~(HID_SELECT | HID_START);
    s.buttons |= mcu_buttons();     /* HOME / POWER from the MCU (mcu.c) */

    if (m->tsc_ok) {
        bool pen;
        u16 tx, ty;
        int16_t cx, cy;
        if (tsc_read(&pen, &tx, &ty, &cx, &cy)) {
            s.flags = INFLAG_CPAD | (pen ? INFLAG_PEN : 0);
            s.cx = cx;
            s.cy = cy;
            if (pen) { s.tx = tx; s.ty = ty; }
        }
    }

    bool moved = (s.flags & INFLAG_PEN) &&
                 absdiff(s.tx, last.tx) + absdiff(s.ty, last.ty) >= MOVE_THRESH;
    bool changed = s.buttons != last.buttons ||
                   ((s.flags ^ last.flags) & INFLAG_PEN) || moved;
    m->polls = tick;
    if (!changed) return;
    if ((s.flags & INFLAG_PEN) && !moved && (last.flags & INFLAG_PEN)) {
        s.tx = last.tx;             /* button-only change: keep the pen still */
        s.ty = last.ty;
    }

    u32 head = m->head;
    volatile insample_t *d = &m->s[head % INPUT_RING];
    d->buttons = s.buttons;
    d->flags = s.flags;
    d->tx = s.tx;
    d->ty = s.ty;
    d->cx = s.cx;
    d->cy = s.cy;
    d->tick = s.tick;
    m->head = head + 1;             /* publish last */
    last = s;
}
