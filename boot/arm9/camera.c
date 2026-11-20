/*
 * ios3ds ARM9: the 3DS cameras' control side (docs/camera.md).
 *
 * The ARM11 kernel shim (kernel/shim/cam.c) asks for a camera through the
 * camera mailbox (cammbox_t at shim + SHIM_CAM_OFF, boot/common/cam_proto.h);
 * this file, polled from the monitor loop, does what needs the I2C buses:
 *   - CFG11_CAMERA_CNT bit 0 (the cameras' 16 MHz clock, needed for their I2C),
 *   - the Aptina MT9V113 bring-up: reset, wakeup, PLL, YUV422 output at 320x240
 *     (GBATEK "DSi Aptina Camera Initialization", with output size A set to
 *     320x240), parallel output on,
 *   - standby of the camera that is not wanted (the inner camera and the outer
 *     right camera share the CAM0 parallel bus: only one may drive it).
 * The shim then owns the CAM block (CAM_CNT, overrun recovery) and drains its
 * FIFO, which only the ARM11 can read.
 *
 * Which camera sits where (GBATEK "3DS I2C", "3DS Video CAM Registers"):
 *   inner camera      I2C bus 0x10161000, device 0x7A, CAM0 (0x10120000)
 *   outer right       I2C bus 0x10161000, device 0x78, CAM0 (unused by iOS: kept in standby)
 *   outer left        I2C bus 0x10144000, device 0x78, CAM1 (0x10121000)
 * Nothing here has run on a console yet (docs/camera.md section 6).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "camera.h"
#include "i2c.h"
#include "log.h"
#include "cam_proto.h"

#define BUS_DSI         0           /* i2c.c bus numbers: 0 = 0x10161000, 1 = 0x10144000 */
#define BUS_3DS         1
#define REG32(a)        (*(vu32 *)(a))
#define REG8(a)         (*(vu8 *)(a))

enum { CAMERR_NONE, CAMERR_NO_ID, CAMERR_INIT, CAMERR_WAKE, CAMERR_PLL, CAMERR_SEQ };

static volatile cammbox_t *box;
static bool enabled;
static u32 handled_seq;
static int active = -1;             /* camera whose parallel output is on */
static bool powered;

#define T9_VAL(n)   (*(vu16 *)(0x10003000u + (n) * 4))
static u32 t9(void)                 /* main.c's free-running 65457 Hz clock */
{
    u16 hi, lo, hi2;
    do {
        hi = T9_VAL(3);
        lo = T9_VAL(2);
        hi2 = T9_VAL(3);
    } while (hi != hi2);
    return (u32)hi << 16 | lo;
}
static void wait_ms(u32 ms)
{
    u32 t0 = t9();
    while (t9() - t0 < ms * 66u) { }
}

typedef struct { u8 bus, dev, port; const char *name; } camdev_t;
static const camdev_t cams[3] = {
    [CAM_OUTER] = { BUS_3DS, CAM_I2C_OUTER_L, 1, "outer left" },
    [CAM_INNER] = { BUS_DSI, CAM_I2C_INNER, 0, "inner" },
    [2]         = { BUS_DSI, CAM_I2C_OUTER_R, 0, "outer right" },
};

/* ---- Aptina register access (16-bit index, 16-bit big-endian data) ---- */
static bool apt_wr(const camdev_t *c, u16 idx, u16 v)
{
    u8 b[2] = { (u8)(v >> 8), (u8)v };
    return i2c_write16(c->bus, c->dev, idx, b, 2);
}
static bool apt_rd(const camdev_t *c, u16 idx, u16 *v)
{
    u8 b[2];
    if (!i2c_read16(c->bus, c->dev, idx, b, 2)) return false;
    *v = (u16)(b[0] << 8 | b[1]);
    return true;
}
static bool apt_mask(const camdev_t *c, u16 idx, u16 clr, u16 set)
{
    u16 v;
    return apt_rd(c, idx, &v) && apt_wr(c, idx, (u16)((v & ~clr) | set));
}
/* MCU variables through XDMA: MCU_ADDRESS 0x098C, MCU_DATA_0 0x0990 */
static bool mcu_wr(const camdev_t *c, u16 var, u16 v)
{
    return apt_wr(c, 0x098C, var) && apt_wr(c, 0x0990, v);
}
static bool mcu_rd(const camdev_t *c, u16 var, u16 *v)
{
    return apt_wr(c, 0x098C, var) && apt_rd(c, 0x0990, v);
}
/* wait until (reg & mask) == want, at most 100 ms */
static bool apt_wait(const camdev_t *c, u16 idx, u16 mask, u16 want, bool mcu)
{
    u32 t0 = t9();
    do {
        u16 v;
        if ((mcu ? mcu_rd(c, idx, &v) : apt_rd(c, idx, &v)) && (v & mask) == want) return true;
    } while (t9() - t0 < 6546u);
    return false;
}

/* GBATEK aptina_code_list_init, output size A = 320x240 */
static u32 apt_init(const camdev_t *c, bool inner)
{
    static const u16 seq1[][2] = {          /* MCU variable writes */
        { 0x02F0, 0x0000 }, { 0x02F2, 0x0210 }, { 0x02F4, 0x001A }, { 0x2145, 0x02F4 }, { 0xA134, 0x0001 },
    };
    static const u16 modes[][2] = {
        { 0x2755, 0x0002 }, { 0x2757, 0x0002 },                         /* output format A/B: YUV */
    };
    static const u16 sizes[][2] = {
        { 0x2703, CAM_W }, { 0x2705, CAM_H },                           /* output size A */
        { 0x2707, 0x0280 }, { 0x2709, 0x01E0 },                         /* output size B 640x480 */
        { 0x2715, 0x0001 }, { 0x2719, 0x001A }, { 0x271B, 0x006B }, { 0x271D, 0x006B },
        { 0x271F, 0x02C0 }, { 0x2721, 0x034B },                         /* sensor A timing */
        { 0xA20B, 0x0000 }, { 0xA20C, 0x0006 },                         /* AE index range */
        { 0x272B, 0x0001 }, { 0x272F, 0x001A }, { 0x2731, 0x006B }, { 0x2733, 0x006B },
        { 0x2735, 0x02C0 }, { 0x2737, 0x034B },                         /* sensor B timing */
    };
    u16 v;
    apt_wr(c, 0x001A, 0x0003);              /* reset */
    apt_wr(c, 0x001A, 0x0000);
    apt_wr(c, 0x0018, 0x4028);              /* wakeup */
    apt_wr(c, 0x001E, 0x0201);
    apt_wr(c, 0x0016, 0x42DF);
    if (!apt_wait(c, 0x0018, 0x4000, 0, false) || !apt_wait(c, 0x301A, 0x0004, 0x0004, false))
        return CAMERR_WAKE;
    for (u32 i = 0; i < sizeof seq1 / sizeof seq1[0]; i++) mcu_wr(c, seq1[i][0], seq1[i][1]);
    if (mcu_rd(c, 0xA115, &v)) mcu_wr(c, 0xA115, v | 2);               /* SEQ_CAP_MODE: video */
    for (u32 i = 0; i < sizeof modes / sizeof modes[0]; i++) mcu_wr(c, modes[i][0], modes[i][1]);
    apt_wr(c, 0x0014, 0x2145);              /* PLL as on the DSi */
    apt_wr(c, 0x0010, 0x0111);
    apt_wr(c, 0x0012, 0x0000);
    apt_wr(c, 0x0014, 0x244B);
    apt_wr(c, 0x0014, 0x304B);
    if (!apt_wait(c, 0x0014, 0x8000, 0x8000, false)) return CAMERR_PLL;
    apt_mask(c, 0x0014, 0x0001, 0);         /* PLL bypass off */
    for (u32 i = 0; i < sizeof sizes / sizeof sizes[0]; i++) mcu_wr(c, sizes[i][0], sizes[i][1]);
    apt_mask(c, 0x3210, 0, 0x0008);
    mcu_wr(c, 0xA208, 0x0000);
    mcu_wr(c, 0xA24C, 0x0020);
    mcu_wr(c, 0xA24F, 0x0070);
    /* read mode as in GBATEK's DSi list (0x24 for device 0x7A, 0x25 = x-flip for the others);
       which one gives a true picture on the 3DS is unverified (docs/camera.md section 7) */
    mcu_wr(c, 0x2717, inner ? 0x0024 : 0x0025);
    mcu_wr(c, 0x272D, inner ? 0x0024 : 0x0025);
    mcu_wr(c, 0xA202, inner ? 0x0022 : 0x0000);
    mcu_wr(c, 0xA203, inner ? 0x00BB : 0x00FF);
    apt_mask(c, 0x0016, 0, 0x0020);
    mcu_wr(c, 0xA115, 0x0072);
    mcu_wr(c, 0xA11F, 0x0001);
    apt_wr(c, 0x326C, inner ? 0x0900 : 0x1000);
    mcu_wr(c, 0xAB22, inner ? 0x0001 : 0x0002);
    mcu_wr(c, 0xA103, 0x0006);              /* refresh mode */
    if (!apt_wait(c, 0xA103, 0x000F, 0, true)) return CAMERR_SEQ;
    mcu_wr(c, 0xA103, 0x0005);              /* refresh */
    if (!apt_wait(c, 0xA103, 0x000F, 0, true)) return CAMERR_SEQ;
    return CAMERR_NONE;
}

static bool apt_activate(const camdev_t *c)
{
    if (!apt_mask(c, 0x0018, 0x0001, 0)) return false;                  /* out of standby */
    apt_wait(c, 0x0018, 0x4000, 0, false);
    apt_wait(c, 0x301A, 0x0004, 0x0004, false);
    return apt_mask(c, 0x001A, 0, 0x0200);                              /* parallel output on */
}

static void apt_standby(const camdev_t *c)
{
    apt_mask(c, 0x001A, 0x0200, 0);                                     /* parallel output off */
    apt_mask(c, 0x0018, 0, 0x0001);                                     /* standby */
}

static void publish(u32 state, u32 err)
{
    box->err = err;
    box->state = state;
    box->ack_seq = box->req_seq;
}

static void power(bool on)
{
    if (on == powered) return;
    powered = on;
    if (on) {
        REG8(CFG11_CAMERA_CNT_PA) |= 1;     /* the cameras' clock (their I2C needs it) */
        wait_ms(5);
        i2c_init(BUS_DSI);
        i2c_init(BUS_3DS);
        u32 present = 0;
        for (u32 i = 0; i < 3; i++) {
            u16 id = 0;
            bool ok = apt_rd(&cams[i], 0x0000, &id);
            if (ok && id == APTINA_CHIP_ID && i < CAM_COUNT) present |= 1u << i;
            log_ok("[cam] %s camera (I2C %s 0x%02X): %s%04x\n", cams[i].name,
                   cams[i].bus == BUS_DSI ? "0x10161000" : "0x10144000", cams[i].dev,
                   ok ? "chip id " : "no answer, ", ok ? id : 0);
            if (ok && id == APTINA_CHIP_ID && i == 2) apt_standby(&cams[2]);   /* keep CAM0's bus free */
        }
        box->present = present;
    } else {
        REG8(CFG11_CAMERA_CNT_PA) &= (u8)~1u;
    }
}

static void start(u32 cam)
{
    const camdev_t *c = &cams[cam];
    u32 err, t0 = t9();
    publish(CAM_ST_STARTING, 0);
    power(true);
    if (active >= 0 && active != (int)cam) {
        apt_standby(&cams[active]);
        active = -1;
    }
    if (!(box->present & (1u << cam))) {
        log_warn("[cam] %s camera did not answer on I2C: iOS gets no picture\n", c->name);
        publish(CAM_ST_FAILED, CAMERR_NO_ID);
        return;
    }
    err = apt_init(c, cam == CAM_INNER);
    if (!err && !apt_activate(c)) err = CAMERR_INIT;
    if (err) {
        log_warn("[cam] %s camera: init failed (step %u)\n", c->name, (unsigned)err);
        apt_standby(c);
        publish(CAM_ST_FAILED, err);
        return;
    }
    active = (int)cam;
    box->cam = cam;
    box->port = c->port;
    publish(CAM_ST_RUNNING, 0);
    log_ok("[cam] %s camera streaming %ux%u YUV422 into CAM%u (init %u ms)\n", c->name, CAM_W, CAM_H,
           (unsigned)c->port, (unsigned)((t9() - t0) / 65u));
}

static void stop(void)
{
    if (active >= 0) apt_standby(&cams[active]);
    active = -1;
    power(false);
    publish(CAM_ST_OFF, 0);
    log_ok("[cam] cameras off (frames %u, FIFO overruns %u, CAM restarts %u)\n",
           (unsigned)box->frames, (unsigned)box->overruns, (unsigned)box->restarts);
}

void camera_init(u32 shim_pa, bool enable)
{
    if (!shim_pa) return;
    box = (volatile cammbox_t *)(shim_pa + SHIM_CAM_OFF);
    for (u32 i = 0; i < sizeof(cammbox_t) / 4; i++) ((vu32 *)box)[i] = 0;
    enabled = enable;
    box->enabled = enable;
    box->state = enable ? CAM_ST_OFF : CAM_ST_DISABLED;
    handled_seq = 0;
    if (!enable) log_warn("[cam] disabled in boot.cfg (camera=0): iOS sees no camera\n");
}

void camera_poll(void)
{
    static u32 t_stat, frames0, ovr0;
    u32 seq, want;
    if (!box || !enabled || box->magic != CAM_MBOX_MAGIC) return;
    if (active >= 0 && t9() - t_stat > 10u * 65457u) {      /* every 10 s while a camera runs */
        u32 f = box->frames, o = box->overruns;
        t_stat = t9();
        log_ok("[cam] %s camera: %u frames, %u FIFO overruns in the last 10 s\n", cams[active].name,
               (unsigned)(f - frames0), (unsigned)(o - ovr0));
        frames0 = f;
        ovr0 = o;
    }
    seq = box->req_seq;
    if (seq == handled_seq) return;
    handled_seq = seq;
    want = box->want;
    if (want && want - 1 < CAM_COUNT) {
        log_ok("[cam] iOS wants the %s camera\n", cams[want - 1].name);
        start(want - 1);
    } else {
        stop();
    }
}
