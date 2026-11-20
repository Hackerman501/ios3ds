/*
 * ios3ds QEMU board: the 3DS cameras (docs/camera.md section 5).
 *
 *   - three Aptina MT9V113 sensors on I2C (bus 1 0x7A inner, 0x78 outer right;
 *     bus 2 0x78 outer left): a 16-bit register file with the few behaviours
 *     the ARM9 driver waits for (chip id 0x2280, standby status, PLL lock,
 *     sequencer commands done), MCU variables through 0x098C/0x0990, the
 *     output size from MODE_OUTPUT_WIDTH/HEIGHT_A; they only answer while
 *     CFG11_CAMERA_CNT bit 0 (the camera clock) is set;
 *   - the two CAM blocks (0x10120000 / 0x10121000): CAM_CNT, CAM_STAT (FIFO
 *     fill in 8-byte units, overrun), the overrun clear, SOFS/EOFS;
 *   - their FIFO windows (0x10320000 / 0x10321000, 0x80 bytes, ARM11 only):
 *     a word is consumed only when the read address differs from the previous
 *     one (GBATEK: reading one fixed address returns the same word forever);
 *   - a moving test pattern (boot/common/cam_pattern.h, the same picture the
 *     plug-in's pattern mode draws), one scanline at a time on QEMU's virtual
 *     clock at 15 frames per second, into a FIFO of 0x500 units (10 KiB).
 *     A full FIFO sets the overrun bit and stops the transfer until it is
 *     cleared, as GBATEK describes; CAM_CNT bit 15 starts at the next frame.
 *
 * A sensor streams into its port when it is out of standby with its parallel
 * output on (reg 0x001A bit 9). Machine property for tests: cam-frames
 * (frames produced).  IOS3DS_CAM_LOG=1 logs I2C traffic and transfers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "system/memory.h"
#include "ctr_cam.h"
#include "cam_proto.h"
#include "cam_pattern.h"

#define NSENS           3
#define FIFO_WORDS      (0x500 * 2)
#define FPS             15
#define VBLANK_LINES    24

typedef struct {
    int bus;                    /* i2c.c numbering: 0 = 0x10161000, 1 = 0x10144000 */
    uint8_t addr;               /* 8-bit write address */
    int port, cam;              /* CAM block it drives; pattern marker */
    uint16_t reg[0x8000];       /* registers by index / 2 */
    uint16_t var[0x10000 >> 1]; /* MCU variables by (address & 0x7FFF) / 2... see var_slot() */
    uint16_t idx;               /* register pointer */
    int nidx;                   /* index bytes received in this write */
    uint8_t wlatch;             /* high data byte of a 16-bit write */
    int wbytes;
    int rbyte;                  /* read: 0 = high byte next */
} Sensor;

typedef struct {
    MemoryRegion regs, fifo;
    uint32_t cnt, sofs, eofs;
    uint16_t clr;
    bool overrun, armed;        /* armed: transfer enabled, waiting for a frame start */
    uint32_t q[FIFO_WORDS];
    uint32_t head, count;
    hwaddr last_addr;
    uint32_t last_word;
    uint32_t line;              /* next scanline of the frame (H .. H+VBLANK = blanking) */
    uint32_t frame;
    uint64_t pushed, popped;
} Port;

static struct {
    Sensor s[NSENS];
    Port p[2];
    QEMUTimer *timer;
    bool clock_on;
    int cur_bus_dev[3];         /* per I2C bus: addressed sensor, -1 none */
    uint64_t frames;
    bool log;
} K;

#define LOG(...) do { if (K.log) qemu_log(__VA_ARGS__); } while (0)

static uint16_t *var_slot(Sensor *s, uint16_t addr)
{
    return &s->var[(addr & 0x7FFF) >> 1 | (addr & 1 ? 0x4000 : 0)];
}

static void sensor_reset(Sensor *s)
{
    memset(s->reg, 0, sizeof s->reg);
    s->reg[0x0000 / 2] = APTINA_CHIP_ID;
    s->reg[0x0018 / 2] = 0x4029;        /* in standby */
    s->reg[0x0014 / 2] = 0x8000;
}

static bool sensor_awake(Sensor *s) { return !(s->reg[0x0018 / 2] & 1); }

static bool sensor_streams(Sensor *s)
{
    return K.clock_on && sensor_awake(s) && (s->reg[0x001A / 2] & 0x0200);
}

static uint16_t sensor_read(Sensor *s, uint16_t idx)
{
    uint16_t v = s->reg[(idx & 0xFFFE) / 2];
    switch (idx & 0xFFFE) {
    case 0x0018:                        /* standby status follows the request at once */
        v = (v & ~0x4000) | (sensor_awake(s) ? 0 : 0x4000);
        break;
    case 0x301A:
        v = (v & ~0x0004) | (sensor_awake(s) ? 0x0004 : 0);
        break;
    case 0x0014:
        v |= 0x8000;                    /* PLL locked */
        break;
    case 0x0990: {
        uint16_t a = s->reg[0x098C / 2];
        v = (a & 0x7FFF) == (0xA103 & 0x7FFF) ? 0 : *var_slot(s, a);   /* SEQ_CMD: done */
        break;
    }
    }
    return v;
}

static void sensor_write(Sensor *s, uint16_t idx, uint16_t v)
{
    idx &= 0xFFFE;
    if (idx == 0x0990) {
        *var_slot(s, s->reg[0x098C / 2]) = v;
        return;
    }
    if (idx == 0x0000) return;
    if (idx == 0x001A && (v & 1)) {     /* soft reset */
        sensor_reset(s);
        return;
    }
    s->reg[idx / 2] = v;
    LOG("[cam] sensor %d reg %04x = %04x\n", s->cam, idx, v);
}

static void sensor_size(Sensor *s, uint32_t *w, uint32_t *h)
{
    *w = *var_slot(s, 0x2703);
    *h = *var_slot(s, 0x2705);
    if (!*w || *w > 640 || (*w & 1)) *w = 640;
    if (!*h || *h > 480) *h = 480;
}

/* ---- I2C (called by nintendo3ds.c's controller model) --------------------- */
bool ctr_cam_i2c_start(int bus, uint8_t addr)
{
    K.cur_bus_dev[bus] = -1;
    if (!K.clock_on) return false;
    for (int i = 0; i < NSENS; i++) {
        Sensor *s = &K.s[i];
        if (s->bus == bus && s->addr == (addr & 0xFE)) {
            K.cur_bus_dev[bus] = i;
            if (!(addr & 1)) {
                s->nidx = 0;
                s->wbytes = 0;
            } else {
                s->rbyte = 0;
            }
            return true;
        }
    }
    return false;
}

bool ctr_cam_i2c_write(int bus, uint8_t b)
{
    Sensor *s;
    if (K.cur_bus_dev[bus] < 0) return false;
    s = &K.s[K.cur_bus_dev[bus]];
    if (s->nidx < 2) {
        s->idx = s->nidx ? (s->idx & 0xFF00) | b : (uint16_t)(b << 8);
        s->nidx++;
        return true;
    }
    if (!(s->wbytes++ & 1)) {
        s->wlatch = b;
    } else {
        sensor_write(s, s->idx, (uint16_t)(s->wlatch << 8 | b));
        s->idx += 2;
    }
    return true;
}

uint8_t ctr_cam_i2c_read(int bus)
{
    Sensor *s;
    uint16_t v;
    if (K.cur_bus_dev[bus] < 0) return 0xFF;
    s = &K.s[K.cur_bus_dev[bus]];
    v = sensor_read(s, s->idx);
    if (!(s->rbyte++ & 1)) return v >> 8;
    s->idx += 2;
    return v & 0xFF;
}

bool ctr_cam_i2c_present(int bus, uint8_t addr)
{
    for (int i = 0; i < NSENS; i++)
        if (K.s[i].bus == bus && K.s[i].addr == (addr & 0xFE)) return true;
    return false;
}

void ctr_cam_clock(bool on)
{
    if (on != K.clock_on) LOG("[cam] camera clock %s\n", on ? "on" : "off");
    K.clock_on = on;
}

/* ---- data path ---------------------------------------------------------------- */
static Sensor *port_sensor(int port)
{
    for (int i = 0; i < NSENS; i++)
        if (K.s[i].port == port && sensor_streams(&K.s[i])) return &K.s[i];
    return NULL;
}

static void port_line(Port *p, Sensor *s)
{
    uint32_t w, h;
    sensor_size(s, &w, &h);
    if (p->line >= h + VBLANK_LINES) {
        p->line = 0;
        p->frame++;
        K.frames++;
    }
    if (p->line == 0 && p->armed) {     /* the transfer starts with a frame */
        p->armed = false;
        LOG("[cam] port %d: transfer starts at frame %u (%ux%u)\n", (int)(p - K.p), p->frame, w, h);
    }
    if (p->line < h && (p->cnt & CAM_CNT_EN) && !p->armed && !p->overrun) {
        for (uint32_t x = 0; x < w; x += 2) {
            if (p->count == FIFO_WORDS) {
                p->overrun = true;
                LOG("[cam] port %d: FIFO overrun at frame %u line %u\n", (int)(p - K.p), p->frame, p->line);
                break;
            }
            p->q[(p->head + p->count++) % FIFO_WORDS] = cam_pattern_word(s->cam, p->frame, x, p->line, w, h);
            p->pushed++;
        }
    }
    p->line++;
}

static void cam_tick(void *opaque)
{
    uint32_t lines = 480 + VBLANK_LINES;
    Sensor *s0 = port_sensor(0), *s1 = port_sensor(1);
    if (s0 || s1) {
        uint32_t w, h;
        sensor_size(s0 ? s0 : s1, &w, &h);
        lines = h + VBLANK_LINES;
        if (s0) port_line(&K.p[0], s0);
        if (s1) port_line(&K.p[1], s1);
    }
    timer_mod(K.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / FPS / lines);
}

static uint64_t regs_read(void *opaque, hwaddr a, unsigned size)
{
    Port *p = opaque;
    uint32_t v = 0;
    switch (a & ~3) {
    case 0x00: v = p->cnt | ((p->cnt & CAM_CNT_EN) ? 2 : 0); break;
    case 0x04: v = (p->count / 2) | (p->overrun ? CAM_STAT_OVR : 0); v |= (uint32_t)p->clr << 16; break;
    case 0x10: v = p->sofs; break;
    case 0x14: v = p->eofs; break;
    }
    return (v >> ((a & 3) * 8)) & (size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1);
}

static void regs_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    Port *p = opaque;
    int n = (int)(p - K.p);
    if (a == 0x00) {
        uint32_t old = p->cnt;
        p->cnt = (uint32_t)v & 0xEF1C;
        if ((p->cnt & CAM_CNT_EN) && !(old & CAM_CNT_EN)) {
            p->armed = true;
            LOG("[cam] port %d: CAM_CNT %04x: transfer on (waits for a frame start)\n", n, p->cnt);
        } else if (!(p->cnt & CAM_CNT_EN) && (old & CAM_CNT_EN)) {
            p->count = 0;                   /* transfer off drops what is buffered */
            p->armed = false;
        }
    } else if (a == 0x06 && size == 2) {
        if ((v & 0x8000) && !(p->cnt & CAM_CNT_EN)) {
            p->overrun = false;
            p->count = 0;
        }
        p->clr = v & 0x3F;
    } else if (a == 0x10 && size == 4) {
        p->sofs = (uint32_t)v & 0x01FF03FE;
    } else if (a == 0x14 && size == 4) {
        p->eofs = (uint32_t)v & 0x01FF03FE;
    }
}

static uint64_t fifo_read(void *opaque, hwaddr a, unsigned size)
{
    Port *p = opaque;
    if (a == p->last_addr) return p->last_word;
    p->last_addr = a;
    if (p->count) {
        p->last_word = p->q[p->head];
        p->head = (p->head + 1) % FIFO_WORDS;
        p->count--;
        p->popped++;
    } else {
        p->last_word = 0;
    }
    return p->last_word;
}

static void fifo_write(void *opaque, hwaddr a, uint64_t v, unsigned size) { }

static const MemoryRegionOps regs_ops = {
    .read = regs_read, .write = regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};
static const MemoryRegionOps fifo_ops = {
    .read = fifo_read, .write = fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

uint64_t ctr_cam_frames(void) { return K.frames; }

void ctr_cam_init(MemoryRegion *shared, hwaddr regs_off, MemoryRegion *sys)
{
    static const struct { int bus; uint8_t addr; int port, cam; } map[NSENS] = {
        { 0, CAM_I2C_INNER, 0, 1 },     /* inner: pattern marker 2 blocks */
        { 0, CAM_I2C_OUTER_R, 0, 2 },
        { 1, CAM_I2C_OUTER_L, 1, 0 },   /* outer left: 1 block */
    };
    K.log = getenv("IOS3DS_CAM_LOG") && *getenv("IOS3DS_CAM_LOG") == '1';
    for (int i = 0; i < NSENS; i++) {
        K.s[i].bus = map[i].bus;
        K.s[i].addr = map[i].addr;
        K.s[i].port = map[i].port;
        K.s[i].cam = map[i].cam;
        sensor_reset(&K.s[i]);
    }
    for (int i = 0; i < 3; i++) K.cur_bus_dev[i] = -1;
    for (int i = 0; i < 2; i++) {
        Port *p = &K.p[i];
        p->last_addr = ~(hwaddr)0;
        memory_region_init_io(&p->regs, NULL, &regs_ops, p, i ? "ctr.cam1" : "ctr.cam0", 0x20);
        memory_region_add_subregion(shared, regs_off + i * 0x1000, &p->regs);
        memory_region_init_io(&p->fifo, NULL, &fifo_ops, p, i ? "ctr.cam1.fifo" : "ctr.cam0.fifo",
                              CAM_FIFO_WINDOW);
        memory_region_add_subregion_overlap(sys, CAM_FIFO_PA(i), &p->fifo, 1);
    }
    K.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cam_tick, NULL);
    timer_mod(K.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
}
