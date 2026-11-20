/* Host test for tools/ios3ds-input/daemon/n3ds.h and the C-stick swipes in cpad.h */
#include <stdio.h>
#include "../daemon/cpad.h"
#include "../daemon/n3ds.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    cstick_t s = {0};
    cpad_t c = {0};
    cpad_out_t o;
    n3key_t k[4];
    int i, n, moves = 0, stills = 0;

    /* C-stick: dead zone, flick, hysteresis */
    CHECK(cstick_dirs(&s, 40, -50, 1) == 0, "small tilt: nothing");
    CHECK(cstick_dirs(&s, 100, 20, 1) == 2, "right");
    CHECK(cstick_dirs(&s, 40, 0, 1) == 2, "still right inside the hysteresis band");
    CHECK(cstick_dirs(&s, -100, 0, 1) == 2, "no direction change without centring");
    CHECK(cstick_dirs(&s, 10, 10, 1) == 0, "centred");
    CHECK(cstick_dirs(&s, -20, 90, 1) == 4, "up");
    cstick_dirs(&s, 0, 0, 1);
    CHECK(cstick_dirs(&s, 70, -110, 1) == 8, "down wins (dominant axis)");
    CHECK(cstick_dirs(&s, 70, -110, 0) == 0, "invalid: nothing");
    cstick_dirs(&s, 0, 0, 1);
    CHECK(cstick_dirs(&s, -127, 0, 1) == 1, "left");

    /* C-stick down: finger down low, moves up, held still, lifts (no fling) */
    cpad_step(&c, 0, 0, 1, 0, 8, &o);
    CHECK(o.ev == CP_DOWN && o.y > 0.7f && o.x == 0.5f, "scroll starts low (%f)", o.y);
    for (i = 0; i < SWIPE_TICKS + CPAD_STILL + 2; i++) {
        float y = c.y;
        cpad_step(&c, 0, 0, 1, 0, 8, &o);
        if (o.ev == CP_MOVE) { moves++; CHECK(o.y < y, "moves up"); CHECK(stills == 0, "no move after holding still"); }
        if (o.ev == CP_NONE && c.finger) stills++;
        if (o.ev == CP_UP) break;
    }
    CHECK(o.ev == CP_UP && o.y < 0.25f, "lifted high (%f, ev %d)", o.y, o.ev);
    CHECK(moves == SWIPE_TICKS - 1 && stills == CPAD_STILL, "%d moves, %d still ticks", moves, stills);
    cpad_step(&c, 0, 0, 1, 0, 8, &o);
    CHECK(o.ev == CP_NONE, "held stick does not repeat");
    cpad_step(&c, 0, 0, 1, 0, 0, &o);
    cpad_step(&c, 0, 0, 1, 0, 4, &o);
    CHECK(o.ev == CP_DOWN && o.y < 0.3f, "up scrolls the other way (%f)", o.y);

    /* ZL: status-bar tap = down at the top centre, one still tick, up there */
    c = (cpad_t){0};
    cpad_step(&c, 0, 0, 1, 0, 16, &o);
    CHECK(o.ev == CP_DOWN && o.y < 0.03f && o.x == 0.5f, "tap down on the status bar (%f)", o.y);
    cpad_step(&c, 0, 0, 1, 0, 16, &o);
    CHECK(o.ev == CP_NONE && c.finger, "held one tick");
    cpad_step(&c, 0, 0, 1, 0, 16, &o);
    CHECK(o.ev == CP_UP && o.y < 0.03f && !c.finger, "lifted where it went down");
    cpad_step(&c, 0, 0, 1, 0, 16, &o);
    CHECK(o.ev == CP_NONE, "no repeat while held");

    /* ZR: HOME held around a POWER press, with human pauses */
    n = zr_keys(1, k);
    CHECK(n == 4 && k[0].key == N3K_MENU && k[0].down && k[1].key == N3K_POWER && k[1].down &&
          k[2].key == N3K_POWER && !k[2].down && k[3].key == N3K_MENU && !k[3].down, "ZR = HOME + POWER");
    CHECK(k[1].wait_ms >= 50, "POWER after a pause (%u ms)", k[1].wait_ms);
    CHECK(zr_keys(0, k) == 0, "nothing");

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
