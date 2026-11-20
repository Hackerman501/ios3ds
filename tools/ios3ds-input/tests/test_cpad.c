/* Host test for tools/ios3ds-input/daemon/cpad.h */
#include <stdio.h>
#include "../daemon/cpad.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    cpad_t c = {0};
    cpad_out_t o;
    int i, downs = 0, ups = 0, moves = 0;

    /* dead zone: nothing */
    for (i = 0; i < 10; i++) { cpad_step(&c, 200, -250, 1, 0, 0, &o); CHECK(o.ev == CP_NONE, "dead zone tick %d ev %d", i, o.ev); }

    /* stick down: finger down at centre, then moves up (y decreases) */
    cpad_step(&c, 0, -1200, 1, 0, 0, &o);
    CHECK(o.ev == CP_DOWN && o.x == 0.5f && o.y == 0.5f, "down at centre");
    float y0 = o.y;
    cpad_step(&c, 0, -1200, 1, 0, 0, &o);
    CHECK(o.ev == CP_MOVE && o.y < y0, "stick down moves the finger up (y %f)", o.y);

    /* keep tilted: reaches the edge, re-grips (lift, then down at centre again) */
    for (i = 0; i < 200; i++) {
        cpad_step(&c, 0, -1200, 1, 0, 0, &o);
        if (o.ev == CP_DOWN) { downs++; CHECK(o.y == 0.5f, "re-grip at centre"); }
        if (o.ev == CP_UP) { ups++; CHECK(o.y >= CPAD_EDGE_LO - 0.04f, "lift near the edge, not past it (%f)", o.y); }
        if (o.ev == CP_MOVE) { moves++; CHECK(o.y >= CPAD_EDGE_LO, "never moves past the edge (%f)", o.y); }
    }
    CHECK(downs >= 2 && ups >= 2 && moves > 20, "re-grips: %d downs %d ups %d moves", downs, ups, moves);

    /* centre the stick: held still CPAD_STILL ticks, then one lift */
    c = (cpad_t){0};
    cpad_step(&c, 1200, 0, 1, 0, 0, &o);
    cpad_step(&c, 1200, 0, 1, 0, 0, &o);
    CHECK(o.ev == CP_MOVE && o.x < 0.5f, "stick right moves the finger left (%f)", o.x);
    for (i = 0; i < CPAD_STILL - 1; i++) { cpad_step(&c, 0, 0, 1, 0, 0, &o); CHECK(o.ev == CP_NONE, "held still %d", i); }
    cpad_step(&c, 0, 0, 1, 0, 0, &o);
    CHECK(o.ev == CP_UP, "lift after holding still");
    cpad_step(&c, 0, 0, 1, 0, 0, &o);
    CHECK(o.ev == CP_NONE && !c.finger, "idle after lift");

    /* a real touch lifts the virtual finger and suppresses it */
    cpad_step(&c, 0, 1200, 1, 0, 0, &o);
    CHECK(o.ev == CP_DOWN, "down again");
    cpad_step(&c, 0, 1200, 1, 1, 0, &o);
    CHECK(o.ev == CP_UP, "pen lifts the virtual finger");
    cpad_step(&c, 0, 1200, 1, 1, 0, &o);
    CHECK(o.ev == CP_NONE, "nothing while the pen is down");

    /* invalid circle pad data: nothing */
    c = (cpad_t){0};
    cpad_step(&c, 1200, 1200, 0, 0, 0, &o);
    CHECK(o.ev == CP_NONE, "invalid cpad ignored");

    /* D-pad right: one swipe right-to-left of SWIPE_TICKS events ending in a lift */
    c = (cpad_t){0};
    cpad_step(&c, 0, 0, 1, 0, 2, &o);
    CHECK(o.ev == CP_DOWN && o.x > 0.8f, "swipe starts right (%f)", o.x);
    for (i = 0; i < SWIPE_TICKS - 1; i++) { cpad_step(&c, 0, 0, 1, 0, 2, &o); CHECK(o.ev == CP_MOVE, "swipe move %d", i); }
    cpad_step(&c, 0, 0, 1, 0, 2, &o);
    CHECK(o.ev == CP_UP && o.x < 0.2f, "swipe ends left (%f, ev %d)", o.x, o.ev);
    cpad_step(&c, 0, 0, 1, 0, 2, &o);
    CHECK(o.ev == CP_NONE, "held D-pad does not repeat");
    cpad_step(&c, 0, 0, 1, 0, 0, &o);
    cpad_step(&c, 0, 0, 1, 0, 1, &o);
    CHECK(o.ev == CP_DOWN && o.x < 0.2f, "D-pad left swipes the other way (%f)", o.x);

    /* rotation: console turned 90 degrees clockwise (iOS upright) */
    CHECK(dpad_screen(PADB_DOWN, 1) == 1 && dpad_screen(PADB_UP, 1) == 2, "D-pad down/up = screen left/right");
    CHECK(dpad_screen(PADB_LEFT, 1) == 4 && dpad_screen(PADB_RIGHT, 1) == 8, "D-pad left/right = screen up/down");
    CHECK(dpad_screen(PADB_LEFT, 0) == 1 && dpad_screen(PADB_RIGHT, 0) == 2 && dpad_screen(PADB_UP, 0) == 4,
          "norotate keeps console directions");
    {
        int sx, sy;
        stick_screen(0, 1000, 1, &sx, &sy);
        CHECK(sx == 1000 && sy == 0, "stick up = screen right (%d,%d)", sx, sy);
        stick_screen(1000, 0, 1, &sx, &sy);
        CHECK(sx == 0 && sy == -1000, "stick right = screen down (%d,%d)", sx, sy);
        stick_screen(-1000, 0, 1, &sx, &sy);
        CHECK(sy == 1000, "stick left = screen up");
        stick_screen(3, 4, 0, &sx, &sy);
        CHECK(sx == 3 && sy == 4, "norotate: unchanged");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
