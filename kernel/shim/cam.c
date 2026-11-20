/*
 * ios3ds shim: the 3DS cameras' data side and the sysctl "ios3ds_cam"
 * (docs/camera.md). Lives in the shim's second part (link.ld: *pica*.o and
 * *cam*.o go to gpu.bin), called through gpu_glue.c.
 *
 *   thread context (sysctl, from mediaserverd's vcd.mediacapture):
 *     START cam -> frame buffers (IOMallocContiguous, once), IO mappings, and
 *                  the camera mailbox: the ARM9 powers the sensor and sets it
 *                  up over I2C (boot/arm9/camera.c)
 *     FRAME     -> copyout of the newest complete frame (YUV422 320x240)
 *     STOP, INFO
 *   IRQ context (shim_cam_service, from the audio service on every timer and
 *   VBlank interrupt; it asks for a wake-up every CAM_STEP_TICKS while a
 *   camera runs):
 *     once the ARM9 reports the sensor streaming: CAM_CNT on, then drain the
 *     CAM FIFO (ARM11-only window at 0x1032x000, read at increasing addresses
 *     in 32-word bursts) into the frame being filled; a full frame becomes
 *     the newest one. An overrun (CAM_STAT bit 15) loses the frame: transfer
 *     off, overrun cleared, transfer on again (it restarts at a frame start).
 *
 * Three buffers: one being filled (IRQ), the newest complete one, and one the
 * sysctl may be copying out; the IRQ never writes the last two.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "cam_proto.h"
#include "gpu_mirror.h"

#ifndef CAM_HOST_TEST
struct sysctl_oid {
    void *oid_parent, *oid_link;
    int oid_number, oid_kind;
    void *oid_arg1;
    int oid_arg2;
    const char *oid_name;
    int (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};
#define CTLFLAG_RD      0x80000000
#define CTLTYPE_NODE    1
#define OID_AUTO        (-1)
#define EFAULT 14
#define EINVAL 22
#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))    /* Thumb */
#define KPA(addr, ret, ...) ((ret (*)(__VA_ARGS__))(addr))          /* ARM */
#define sysctl_register_oid   KPT(0x80159c54, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPT(0x80159242, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_ROOT_CHILDREN  ((void *)0x8027f7a8)
#define k_copyin              KPA(0x80067c84, int, u32, void *, u32)
#define k_copyout             KPA(0x80067d04, int, const void *, u32, u32)
#define IOMallocContiguous    KPT(0x801a70e0, void *, u32, u32, uint32_t *)
#define k_printf              KPT(0x8001c4c4, void, const char *, ...)

#define PHYS_BASE   0x20000000u
#define VIRT_BASE   0x80000000u
#define PRIV_VA     0xBFF00000u
#define WD_COUNT    0x624
#define IO_PA       0x10100000u     /* CAM registers at +0x20000 (same section as audio.c's) */
#define IO_VA       0xBFC00000u
#define FIFO_PA     0x10300000u     /* CAM FIFO windows at +0x20000 */
#define FIFO_VA     0xBFB00000u
#define MBOX        ((volatile cammbox_t *)(SHIM_VA + SHIM_CAM_OFF))
#define REG32(va)   (*(vu32 *)(va))
#define REG16(va)   (*(vu16 *)(va))
u32 shim_now32(void);                                       /* entry.S: 1047311 Hz on every model */
void shim_l2_inv_kva(const volatile void *va, u32 len);     /* entry.S: New 3DS L2 (docs/n3ds.md) */
static inline u32 now_ticks(void) { return shim_now32(); }
static void dinval(const volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    shim_l2_inv_kva(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
}
static void dclean(const volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static inline u32 irq_off(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr | 0x80) : "memory");
    return cpsr;
}
static inline void irq_restore(u32 cpsr) { __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory"); }
#define CAM_REG(p, o)   (IO_VA + (CAM_REG_PA(p) - IO_PA) + (o))
#define cnt_w(p, v)     (REG32(CAM_REG(p, CAM_CNT)) = (v))
#define cnt_r(p)        REG32(CAM_REG(p, CAM_CNT))
#define stat_r(p)       REG16(CAM_REG(p, CAM_STAT))
#define clr_w(p, v)     (REG16(CAM_REG(p, CAM_CLR)) = (v))
#define fifo_r(i)       REG32(FIFO_VA + (CAM_FIFO_PA(C.port) - FIFO_PA) + (i) * 4)
#else   /* host test (tools/ios3ds-camera/tests/test_cam.c): a model CAM block */
extern volatile cammbox_t cam_host_mbox;
void cam_host_cnt_w(u32 v);
u32 cam_host_stat_r(void);
void cam_host_clr_w(u32 v);
u32 cam_host_fifo_r(u32 word);
#define MBOX            (&cam_host_mbox)
#define cnt_w(p, v)     ((p) == C.port ? cam_host_cnt_w(v) : (void)0)
#define cnt_r(p)        0u
#define stat_r(p)       ((p) == C.port ? cam_host_stat_r() : 0u)
#define clr_w(p, v)     ((p) == C.port ? cam_host_clr_w(v) : (void)0)
#define fifo_r(i)       cam_host_fifo_r(i)
static inline void dclean(const volatile void *p, u32 len) { (void)p; (void)len; }
static inline void dinval(const volatile void *p, u32 len) { (void)p; (void)len; }
#endif

#define CAM_STEP_TICKS  1047u       /* 1 ms of the 1.047 MHz time base: the FIFO (10 KiB) lasts ~4 ms at 15 fps */
#define CAM_PROBE_TICKS (1047u * 2000u) /* 2 s without data: next set-up variant */

/* CAM block set-up variants, tried in turn every 2 s until data arrives.
   Run 25 (re-log 70): with 0x8010, +6 = 0x22 and DRQ/IRQ nothing ever arrived
   on either block. New: the DSi's CAM_CNT init bits (GBATEK DSi "Camera Init":
   bits 8-9 = 2 and bit 10 = 1 before the transfer is enabled, bit 11 IRQ);
   the 3DS register is "alike DSi". The ARM9's boot self-test (diag=1,
   boot/arm9/camera.c) runs the same list per sensor and logs which one works.
     0  0x8010             2  0x8E10 (+ bit 11)       4  0xC610 + DRQ, +6 = 0x22
     1  0x8610 (DSi 8-10)  3  0x8610, +6 = 0x22 */
static const u16 cam_var_clr[CAM_NVARIANTS] = CAM_VAR_CLR_LIST;
static const u32 cam_var_cnt[CAM_NVARIANTS] = CAM_VAR_CNT_LIST;
#define CAM_WORDS       (CAM_FRAME_BYTES / 4u)

static struct {
    u8 *buf[CAM_NBUF];
    u32 fill, newest, reading;      /* buffer indices; reading = CAM_NBUF: none */
    u32 off;                        /* words of the frame being filled */
    u32 win;                        /* next word of the FIFO window to read (never reset) */
    u32 seq, newest_seq, newest_time;
    u32 running;                    /* START seen, no STOP */
    u32 streaming;                  /* CAM_CNT is on (the ARM9 said RUNNING) */
    u32 port;
    u32 want_seq;
    u32 io_ok;
    u32 frames, overruns, restarts, words;
    u32 logged;
    u32 variant, probing, t_probe;  /* no data seen yet: both ports on, variants cycled */
    u32 services, data_seen, stat_max, stat_last;
} C = { .reading = CAM_NBUF, .newest = CAM_NBUF };

/* ---- IRQ side ------------------------------------------------------------- */
static void port_start(u32 p)
{
    cnt_w(p, cam_var_cnt[C.variant] & ~CAM_CNT_EN);     /* transfer off, set-up bits on */
    clr_w(p, 0x8000u | cam_var_clr[C.variant]);         /* clear overrun (+ variant bits 0-5) */
    cnt_w(p, cam_var_cnt[C.variant]);                   /* on: the next frame starts at word 0 */
}

static void cnt_restart(void)
{
    port_start(C.port);
    C.off = 0;
}

static u32 next_fill(void)
{
    for (u32 i = 0; i < CAM_NBUF; i++)
        if (i != C.newest && i != C.reading) return i;
    return C.fill;                                      /* not reached with 3 buffers */
}

static void publish_stats(void)
{
    volatile cammbox_t *b = MBOX;
    b->frames = C.frames;
    b->overruns = C.overruns;
    b->restarts = C.restarts;
    b->words = C.words;
    dclean(b, 32);
    b->services = C.services;
    b->data_seen = C.data_seen;
    b->stat_last = C.stat_last;
    b->stat_max = C.stat_max;
    b->cnt_rb = (cnt_r(C.port) & 0xFFFF) | cnt_r(C.port ^ 1) << 16;
    b->variant = C.variant;
    b->port_used = C.port;
    b->probing = C.probing;
    dclean(&b->services, 32);
}

/* No word has arrived since the start: watch both ports (the camera may sit
   on the other CAM block than GBATEK's map says) and cycle the set-up
   variants. Once data is found, C.port points at it and probing ends. */
static void probe(u32 now, u32 st)
{
    u32 o = C.port ^ 1, so = stat_r(o);
    C.stat_last = (st & 0xFFFF) | so << 16;
    if (CAM_STAT_FILL(st) || (st & CAM_STAT_OVR)) {
        cnt_w(o, 0);
        cnt_restart();                                  /* start over at a frame start */
    } else if (CAM_STAT_FILL(so) || (so & CAM_STAT_OVR)) {
        cnt_w(C.port, 0);
        C.port = o;                                     /* data comes in on the other block */
        cnt_restart();
    } else {
        if (now - C.t_probe > CAM_PROBE_TICKS) {
            C.variant = (C.variant + 1) % CAM_NVARIANTS;
            C.t_probe = now;
            port_start(C.port);
            port_start(o);
            publish_stats();
        }
        return;
    }
    C.probing = 0;
    publish_stats();
}

/* Returns the tick at which it wants to run again, 0 = no need. */
u32 shim_cam_service(u32 now)
{
    volatile cammbox_t *b = MBOX;
    u32 st, units;
    if (!C.running) {
        if (C.streaming) {
            cnt_w(0, 0);
            cnt_w(1, 0);
            C.streaming = 0;
        }
        return 0;
    }
    if (!C.streaming) {
        dinval(&b->state, 32);
        if (b->ack_seq != C.want_seq || b->state != CAM_ST_RUNNING) return now + CAM_STEP_TICKS * 10;
        C.port = b->port & 1;
        C.streaming = 1;
        C.fill = next_fill();
        C.probing = 1;
        C.variant = 0;
        C.t_probe = now;
        cnt_restart();
        port_start(C.port ^ 1);
        publish_stats();
        return now + CAM_STEP_TICKS;
    }
    st = stat_r(C.port);
    C.services++;
    if (CAM_STAT_FILL(st)) C.data_seen++;
    if (CAM_STAT_FILL(st) > C.stat_max) C.stat_max = CAM_STAT_FILL(st);
    if (!(C.services & 1023)) {
        C.stat_last = (st & 0xFFFF) | (u32)stat_r(C.port ^ 1) << 16;
        publish_stats();
    }
    if (C.probing) {                                    /* drained from the next service on */
        probe(now, st);
        return now + CAM_STEP_TICKS;
    }
    if (st & CAM_STAT_OVR) {
        C.overruns++;
        C.restarts++;
        cnt_restart();
        publish_stats();
        return now + CAM_STEP_TICKS;
    }
    units = CAM_STAT_FILL(st);
    if (units > CAM_FIFO_UNITS) units = CAM_FIFO_UNITS;
    for (u32 w = units * 2; w;) {
        u32 n = w, room = CAM_WORDS - C.off;
        u32 *dst = (u32 *)C.buf[C.fill] + C.off;
        if (n > room) n = room;
        /* the window is read at increasing addresses and wraps, carrying on
           where the last read stopped: a word is consumed only when the
           address changes (GBATEK), so two reads in a row at the same
           address would return the same word */
        for (u32 i = 0; i < n; i++) {
            dst[i] = fifo_r(C.win);
            C.win = (C.win + 1) & (CAM_FIFO_WINDOW / 4 - 1);
        }
        C.off += n;
        C.words += n;
        w -= n;
        if (C.off == CAM_WORDS) {
            C.newest = C.fill;
            C.newest_seq = ++C.seq;
            C.newest_time = now;
            C.frames++;
            C.fill = next_fill();
            C.off = 0;
            if (!(C.frames & 15)) publish_stats();
        }
    }
    return now + CAM_STEP_TICKS;
}

/* ---- thread side ------------------------------------------------------------ */
#ifndef CAM_HOST_TEST
/* the same 1 MiB device-section recipe as audio.c aud_map_io (shared with it for IO_VA) */
static int map_section(u32 pa, u32 va)
{
    u32 ttbr1, *l1, want = pa | (1u << 10) | (1u << 4) | (1u << 2) | 2u;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(ttbr1));
    l1 = (u32 *)((ttbr1 & ~0x3FFFu) - PHYS_BASE + VIRT_BASE) + (va >> 20);
    if (*l1 == want) return 1;
    if (*l1 != 0) return 0;
    *l1 = want;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(l1) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 1" :: "r"(va) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(0) : "memory");
    return 1;
}

static u32 cam_enabled(void)
{
    volatile cammbox_t *b = MBOX;
    dinval(&b->state, 32);
    return b->enabled && b->state != CAM_ST_DISABLED;
}

static void send_want(u32 want)
{
    volatile cammbox_t *b = MBOX;
    b->magic = CAM_MBOX_MAGIC;
    b->want = want;
    C.want_seq = ++b->req_seq;
    dclean(b, 32);
}

static u32 op_start(u32 cam)
{
    u32 s;
    if (cam >= CAM_COUNT) return CAM_ERR_BAD_OP;
    if (!cam_enabled()) return CAM_ERR_DISABLED;
    if (!C.io_ok) {
        C.io_ok = map_section(IO_PA, IO_VA) && map_section(FIFO_PA, FIFO_VA);
        if (!C.io_ok) {
            k_printf("[cam] no IO window for the CAM block: camera off\n");
            return CAM_ERR_DISABLED;
        }
    }
    for (u32 i = 0; i < CAM_NBUF; i++) {
        uint32_t pa;
        if (!C.buf[i] && !(C.buf[i] = IOMallocContiguous(CAM_FRAME_BYTES, 32, &pa))) return CAM_ERR_NO_MEM;
    }
    s = irq_off();
    C.running = 0;                  /* the IRQ turns CAM_CNT off and waits for the new state */
    C.streaming = 0;
    C.newest = CAM_NBUF;
    C.off = 0;
    send_want(1 + cam);
    C.running = 1;
    irq_restore(s);
    if (C.logged++ < 20)
        k_printf("[cam] start %s camera (frames so far %u, overruns %u)\n", cam ? "inner" : "outer", C.frames, C.overruns);
    return CAM_OK;
}

static u32 op_stop(void)
{
    u32 s = irq_off();
    C.running = 0;
    if (C.streaming) {
        cnt_w(0, 0);
        cnt_w(1, 0);
        C.streaming = 0;
    }
    send_want(0);
    publish_stats();
    irq_restore(s);
    return CAM_OK;
}

static u32 op_frame(cam_req_t *r)
{
    u32 s, idx, seq, t, err;
    volatile cammbox_t *b = MBOX;
    if (!C.running) return CAM_ERR_NOT_RUNNING;
    dinval(&b->state, 32);
    if (b->ack_seq == C.want_seq && b->state == CAM_ST_FAILED) return CAM_ERR_FAILED;
    s = irq_off();
    idx = C.newest;
    seq = C.newest_seq;
    t = C.newest_time;
    if (idx < CAM_NBUF && seq != r->a[1]) C.reading = idx;
    irq_restore(s);
    if (idx >= CAM_NBUF || seq == r->a[1]) return CAM_ERR_AGAIN;
    err = k_copyout(C.buf[idx], r->a[0], CAM_FRAME_BYTES);
    C.reading = CAM_NBUF;
    if (err) return CAM_ERR_FAULT;
    r->a[1] = seq;
    r->a[2] = t;
    return CAM_OK;
}

static int cam_sysctl(struct sysctl_oid *oidp, void *arg1, int namelen, void *req)
{
    int *name = (int *)arg1;
    cam_req_t r;
    if (namelen == 0) {
        cam_info_t info = { CAM_PROTO_VERSION, cam_enabled(), MBOX->state, C.frames, C.overruns, MBOX->present };
        return sysctl_handle_opaque(oidp, &info, sizeof info, req);
    }
    if (namelen != 1) return EINVAL;
    if (k_copyin((u32)name[0], &r, sizeof r)) return EFAULT;
    switch (r.op) {
    case CAM_OP_INFO: {
        cam_info_t *i = (cam_info_t *)r.a;
        i->version = CAM_PROTO_VERSION;
        i->enabled = cam_enabled();
        i->state = MBOX->state;
        i->frames = C.frames;
        i->overruns = C.overruns;
        i->cams = MBOX->present;
        r.status = CAM_OK;
        break;
    }
    case CAM_OP_START: r.status = op_start(r.a[0]); break;
    case CAM_OP_STOP:  r.status = op_stop(); break;
    case CAM_OP_FRAME: r.status = op_frame(&r); break;
    default:           r.status = CAM_ERR_BAD_OP; break;
    }
    if (k_copyout(&r, (u32)name[0], sizeof r)) return EFAULT;
    return 0;
}

static struct sysctl_oid cam_oid = {
    .oid_parent  = SYSCTL_ROOT_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_NODE),
    .oid_name    = CAM_SYSCTL_NAME,
    .oid_handler = cam_sysctl,
    .oid_fmt     = "N",
};

void shim_cam_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&cam_oid);
}
#else
/* host test hooks */
void cam_test_start(u8 *b0, u8 *b1, u8 *b2)
{
    C.buf[0] = b0;
    C.buf[1] = b1;
    C.buf[2] = b2;
    C.running = 1;
    C.want_seq = MBOX->ack_seq;
}
u32 cam_test_newest(u32 *seq, u32 *reading)
{
    *seq = C.newest_seq;
    if (reading) C.reading = *reading;
    return C.newest;
}
void cam_test_stats(u32 *frames, u32 *overruns) { *frames = C.frames; *overruns = C.overruns; }
u32 cam_test_variant(u32 *probing) { *probing = C.probing; return C.variant; }
#endif
