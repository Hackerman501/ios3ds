/*
 * Host test for the camera path's pure logic (docs/camera.md):
 *   - the shim's FIFO drain (kernel/shim/cam.c built with -DCAM_HOST_TEST)
 *     against a model CAM block that behaves as GBATEK describes and as the
 *     QEMU model does: the transfer starts at a frame start, the FIFO holds
 *     0x500 8-byte units, a word is consumed only when the read address
 *     changes, a full FIFO is an overrun that stops the transfer until
 *     cleared. Every frame the shim completes must be exactly one pattern
 *     frame, also across overruns, uneven service intervals and a reader
 *     holding a buffer;
 *   - the YUV422 -> BGRA conversion and the test pattern (cam_pattern.h).
 * Run by tests/test_camera.py (make test).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ios3ds.h"
#include "cam_proto.h"
#include "cam_pattern.h"

u32 shim_cam_service(u32 now);
void cam_test_start(u8 *b0, u8 *b1, u8 *b2);
u32 cam_test_newest(u32 *seq, u32 *reading);
void cam_test_stats(u32 *frames, u32 *overruns);
u32 cam_test_resyncs(void);
u32 cam_test_variant(u32 *probing);

#define FIFO_WORDS (CAM_FIFO_UNITS * 2)
volatile cammbox_t cam_host_mbox;
static struct {
    u32 cnt, q[FIFO_WORDS], head, count, last_i, last_w, frame, line, armed, overrun, dead;
    u32 popped, lossy;
} M = { .last_i = ~0u };

void cam_host_cnt_w(u32 v)
{
    if ((v & CAM_CNT_EN) && !(M.cnt & CAM_CNT_EN)) M.armed = 1;
    if (!(v & CAM_CNT_EN)) { M.count = 0; M.armed = 0; }
    M.cnt = v;
}
u32 cam_host_stat_r(void) { return M.count / 2 | (M.overrun ? CAM_STAT_OVR : 0); }
void cam_host_clr_w(u32 v) { if ((v & 0x8000) && !(M.cnt & CAM_CNT_EN)) { M.overrun = 0; M.count = 0; } }
u32 cam_host_fifo_r(u32 i)
{
    if (i == M.last_i) return M.last_w;
    M.last_i = i;
    if (!M.count) return M.last_w = 0xDEADBEEF;
    M.last_w = M.q[M.head];
    M.head = (M.head + 1) % FIFO_WORDS;
    M.count--;
    M.popped++;
    return M.last_w;
}

/* the sensor: one scanline per call (frames of CAM_H lines + 20 blanking lines) */
static void model_line(void)
{
    if (M.line == CAM_H + 20) { M.line = 0; M.frame++; }
    if (M.line == 0 && !M.dead) M.armed = 0;   /* a dead sensor sends no frame start */
    if (M.line < CAM_H && (M.cnt & CAM_CNT_EN) && !M.armed && !M.overrun && !M.dead) {
        for (u32 x = 0; x < CAM_W; x += 2) {
            if (M.count == FIFO_WORDS) { M.overrun = 1; break; }
            /* lossy: like run 30, words vanish (every 3rd frame, 8 words) or
               come twice (every 5th frame) without the overrun bit */
            if (M.lossy && M.frame % 3 == 0 && M.line == 37 && x < 16) continue;
            if (M.lossy && M.frame % 5 == 1 && M.line == 50 && x < 8)
                M.q[(M.head + M.count++) % FIFO_WORDS] = cam_pattern_word(0, M.frame, x, M.line, CAM_W, CAM_H);
            M.q[(M.head + M.count++) % FIFO_WORDS] = cam_pattern_word(0, M.frame, x, M.line, CAM_W, CAM_H);
        }
    }
    M.line++;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u8 bufs[3][CAM_FRAME_BYTES];

/* which pattern frame is buffer b? ~0 if none */
static u32 which_frame(const u32 *b, u32 hint)
{
    for (u32 n = hint > 3 ? hint - 3 : 0; n < hint + 4; n++) {
        u32 ok = 1;
        for (u32 y = 0; y < CAM_H && ok; y += 7)
            for (u32 x = 0; x < CAM_W && ok; x += 2)
                ok = b[(y * CAM_W + x) / 2] == cam_pattern_word(0, n, x, y, CAM_W, CAM_H);
        if (ok) return n;
    }
    return ~0u;
}

static void test_drain(void)
{
    u32 now = 0, seq_seen = 0, good = 0, last_frame = 0, frames, overruns;
    srand(1);
    cam_host_mbox.state = CAM_ST_RUNNING;
    cam_host_mbox.port = 1;
    cam_test_start(bufs[0], bufs[1], bufs[2]);
    {   /* no data for 5 s: the shim keeps probing and moves on to other set-up variants */
        u32 probing, v;
        M.dead = 1;
        for (u32 i = 0; i < 5000; i++) { model_line(); shim_cam_service(now += 1047); }
        v = cam_test_variant(&probing);
        CHECK(probing && v == 2, "no data: variant %u, probing %u (want 2, 1)", v, probing);
        M.dead = 0;
    }
    for (u32 step = 0; step < 40000; step++) {
        /* between services: usually 1-4 lines, sometimes a long gap (IRQs masked) */
        u32 lines = (step % 997 == 500) ? 40 : 1 + (u32)(rand() % 4);
        for (u32 i = 0; i < lines; i++) model_line();
        now += 1047;
        shim_cam_service(now);
        if (step % 3 == 0) {
            u32 seq, r = 3, idx = cam_test_newest(&seq, NULL);
            if (idx < 3 && seq != seq_seen) {
                u32 f;
                r = idx;
                cam_test_newest(&seq, &r);          /* the reader holds it ... */
                for (u32 i = 0; i < 30; i++) model_line();
                shim_cam_service(now += 1047);      /* ... while frames go on */
                f = which_frame((const u32 *)bufs[idx], M.frame);
                CHECK(f != ~0u, "completed frame #%u is not a whole pattern frame", seq);
                CHECK(f == ~0u || f > last_frame || !good, "frame %u after %u: out of order", f, last_frame);
                if (f != ~0u) { good++; last_frame = f; }
                seq_seen = seq;
                r = 3;
                cam_test_newest(&seq, &r);          /* released */
            }
        }
    }
    {   /* lossy FIFO: every completed frame must still be whole, the bad ones dropped */
        u32 r0 = cam_test_resyncs(), lgood = 0, lbad = 0;
        M.lossy = 1;
        for (u32 step = 0; step < 30000; step++) {
            for (u32 i = 0, lines = 1 + (u32)(rand() % 3); i < lines; i++) model_line();
            shim_cam_service(now += 1047);
            if (step % 3 == 0) {
                u32 seq, r = 3, idx = cam_test_newest(&seq, NULL);
                if (idx < 3 && seq != seq_seen) {
                    r = idx;
                    cam_test_newest(&seq, &r);
                    if (which_frame((const u32 *)bufs[idx], M.frame) != ~0u) lgood++; else lbad++;
                    seq_seen = seq;
                    r = 3;
                    cam_test_newest(&seq, &r);
                }
            }
        }
        M.lossy = 0;
        CHECK(lbad == 0 && lgood > 50, "lossy FIFO: %u whole frames, %u misaligned ones delivered", lgood, lbad);
        CHECK(cam_test_resyncs() - r0 > 50, "lossy FIFO: only %u resyncs", cam_test_resyncs() - r0);
        printf("lossy FIFO: %u whole frames delivered, 0 misaligned, %u dropped at the vertical blank\n",
               lgood, cam_test_resyncs() - r0);
    }
    {   /* the sensor pauses mid-frame now and then (QEMU under load): not a blank */
        u32 pgood = 0, pbad = 0, r0 = cam_test_resyncs();
        for (u32 step = 0; step < 30000; step++) {
            if (step % 41 != 7 && step % 41 != 8 && step % 41 != 9)   /* 3 services without a line */
                for (u32 i = 0, lines = 1 + (u32)(rand() % 3); i < lines; i++) model_line();
            shim_cam_service(now += 1047);
            if (step % 3 == 0) {
                u32 seq, r = 3, idx = cam_test_newest(&seq, NULL);
                if (idx < 3 && seq != seq_seen) {
                    r = idx;
                    cam_test_newest(&seq, &r);
                    if (which_frame((const u32 *)bufs[idx], M.frame) != ~0u) pgood++; else pbad++;
                    seq_seen = seq;
                    r = 3;
                    cam_test_newest(&seq, &r);
                }
            }
        }
        CHECK(pbad == 0 && pgood > 100, "mid-frame pauses: %u whole frames, %u misaligned", pgood, pbad);
        CHECK((cam_test_resyncs() - r0) * 10 < pgood, "mid-frame pauses: %u frames dropped", cam_test_resyncs() - r0);
        printf("mid-frame pauses: %u whole frames, %u dropped\n", pgood, cam_test_resyncs() - r0);
    }
    cam_test_stats(&frames, &overruns);
    CHECK(good > 100, "only %u good frames", good);
    CHECK(overruns > 10, "the test should provoke overruns (got %u)", overruns);
    printf("drain: %u frames completed, %u checked whole and in order, %u overruns\n", frames, good, overruns);
}

static void test_convert(void)
{
    u32 src[2] = { cam_yuyv(255, 0, 128, 128), cam_yuyv(76, 76, 85, 255) };   /* white/black, red */
    u32 dst[4];
    cam_yuyv_to_bgra(src, 4, 1, (u8 *)dst, 16);
    CHECK(dst[0] == 0xFFFFFFFFu && dst[1] == 0xFF000000u, "white/black %08x %08x", dst[0], dst[1]);
    CHECK((dst[2] >> 16 & 0xFF) > 0xF0 && (dst[2] >> 8 & 0xFF) < 0x10 && (dst[2] & 0xFF) < 0x10,
          "red %08x", dst[2]);
    {   /* 2x upscale: copies on even pixels, averages between, edges repeat */
        u32 in[4] = { 0xFF000000u, 0xFF0000FEu, 0xFF00FE00u, 0xFFFE0000u }, out[16];
        cam_bgra_upscale2x((const u8 *)in, 8, 2, 2, (u8 *)out, 16);
        CHECK(out[0] == in[0] && out[2] == in[1] && out[8] == in[2], "upscale copies");
        CHECK(out[1] == 0xFF00007Fu, "upscale horizontal average %08x", out[1]);
        CHECK(out[4] == 0xFF007F00u, "upscale vertical average %08x", out[4]);
        CHECK(out[3] == in[1] && out[15] == in[3], "upscale edges %08x %08x", out[3], out[15]);
    }
    /* the pattern: camera marker blocks and the moving square */
    CHECK(cam_pattern_word(1, 0, 8, 10, 320, 240) == cam_yuyv(16, 16, 128, 128), "marker 1");
    CHECK(cam_pattern_word(1, 0, 32, 10, 320, 240) == cam_yuyv(16, 16, 128, 128), "marker 2 (inner)");
    CHECK(cam_pattern_word(0, 0, 32, 10, 320, 240) != cam_yuyv(16, 16, 128, 128), "no marker 2 (outer)");
    CHECK(cam_pattern_word(0, 0, 38, 100, 320, 240) != cam_pattern_word(0, 1, 38, 100, 320, 240), "frames differ");
    CHECK(cam_pattern_word(0, 7, 160, 120, 320, 240) == cam_yuyv(47, 47, 128, 128) &&
          cam_pattern_word(1, 167, 130, 97, 320, 240) == cam_yuyv(47, 47, 128, 128), "frame counter patch");
}

int main(void)
{
    test_convert();
    test_drain();
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
