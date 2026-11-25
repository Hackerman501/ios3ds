/* Host test for the auto-brightness curve (docs/mcu.md section 13).
   Build: cc -Wall -Wextra -O1 -Iboot/common -o build/test_als tests/test_als.c
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "als_curve.h"

static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); } } while (0)

int main(void)
{
    /* 1. luminance -> lux is monotonic and anchored */
    float prev = -1;
    for (unsigned l = 0; l < 256; l++) {
        float lux = als_luma_to_lux((uint8_t)l);
        CHECK(lux >= prev - 1e-6f, "luma %u: lux %.3f < %.3f", l, lux, prev);
        CHECK(lux >= 0 && lux <= ALS_LUX_MAX, "luma %u out of range", l);
        prev = lux;
    }
    CHECK(als_luma_to_lux(0) == 0, "dark is 0 lux");
    CHECK(fabsf(als_luma_to_lux(255) - 3000.0f) < 0.01f, "full luma is 3000 lux");
    CHECK(fabsf(als_luma_to_lux(160) - 100.0f) < 1.0f, "luma 160 ~ 100 lux, got %.1f", als_luma_to_lux(160));

    /* 2. brightness factor is bounded and rises with ambient light */
    prev = -1;
    for (float lux = 0.1f; lux <= 10000; lux *= 1.1f) {
        float f = als_lux_to_factor(lux);
        CHECK(f >= ALS_FACTOR_MIN - 1e-6f && f <= ALS_FACTOR_MAX + 1e-6f,
              "factor %.3f out of range at %.1f lux", f, lux);
        CHECK(f >= prev - 1e-6f, "factor fell from %.3f to %.3f at %.1f lux", prev, f, lux);
        prev = f;
    }
    CHECK(fabsf(als_lux_to_factor(0) - 0.45f) < 1e-6f, "dark floor");
    CHECK(fabsf(als_lux_to_factor(3000) - 1.0f) < 1e-6f, "bright ceiling");

    /* 3. first sample snaps, later ones are smoothed */
    als_smooth_t st = { 0 };
    float f = als_smooth(&st, 160, 100);                 /* ~100 lux -> 0.80 */
    CHECK(fabsf(f - 0.80f) < 0.02f, "initial factor %.3f", f);
    float before = f;
    f = als_smooth(&st, 0, 100);                         /* darkness */
    CHECK(f <= before && f >= before - 0.02f, "one sample must not jump: %.3f -> %.3f", before, f);
    for (int i = 0; i < 200; i++) f = als_smooth(&st, 0, 100);
    CHECK(fabsf(f - ALS_FACTOR_MIN) < 0.001f, "converges to dark floor, got %.3f", f);

    /* 4. one-sample flicker is ignored, a sustained change is followed */
    als_smooth_t fl = { 0 };
    als_smooth(&fl, 160, 100);                           /* 0.80 */
    float steady = fl.current;
    als_smooth(&fl, 255, 100);                           /* single bright frame */
    als_smooth(&fl, 160, 100);
    for (int i = 0; i < 5; i++) als_smooth(&fl, 160, 100);
    CHECK(fabsf(fl.current - steady) < 0.03f, "flicker moved the backlight to %.3f", fl.current);
    for (int i = 0; i < 60; i++) als_smooth(&fl, 255, 100);
    CHECK(fl.current > 0.95f, "sustained light not followed: %.3f", fl.current);

    /* 5. no oscillation around a steady light level */
    for (int i = 0; i < 100; i++) als_smooth(&fl, 255, 100);
    float settled = fl.current;
    for (int i = 0; i < 20; i++) {
        als_smooth(&fl, 254, 100);
        als_smooth(&fl, 255, 100);
    }
    CHECK(fabsf(fl.current - settled) < 0.02f, "jitter oscillates: %.3f -> %.3f", settled, fl.current);

    printf("test_als: %d checks OK\n", checks);
    return 0;
}
