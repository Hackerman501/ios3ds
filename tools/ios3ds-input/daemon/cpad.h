/* Circle pad and D-pad -> touch gestures (no cursor in iOS 4, so the stick
 * scrolls whatever is under the screen centre).
 *
 * Circle pad: a virtual finger goes down at the centre and moves against the
 * stick (stick down = finger up = the content scrolls to show what is below),
 * speed proportional to the tilt past a dead zone. Near the screen edge the
 * finger is held still, lifted and put down at the centre again (re-grip).
 * Back in the dead zone the finger is held still for a few ticks before it
 * lifts, so UIScrollView sees no velocity and does not fling.
 * D-pad left/right: one page swipe (home screen pages, Photos).
 * Up/down "swipes" (the New 3DS C-stick, n3ds.h): the finger moves 0.6 of
 * the screen and is held still before it lifts, so a list scrolls by about
 * that much and does not fling.
 * Status-bar tap (the New 3DS ZL, n3ds.h): a tap at the top centre, which
 * makes iOS scroll the frontmost list to its top.
 * A real touch always wins: the virtual finger lifts and nothing is
 * synthesised while the pen is down.
 *
 * Pure logic, one call per poll tick (16 ms); host-tested by
 * tools/ios3ds-input/tests/test_cpad.c. docs/input-design.md section 8. */
#ifndef IOS3DS_CPAD_H
#define IOS3DS_CPAD_H

#define CPAD_DEAD     300       /* raw ADC units around centre (QEMU full tilt 1200) */
#define CPAD_FULL     1200
#define CPAD_SPEED    0.030f    /* normalised screen units per tick at full tilt */
#define CPAD_STILL    4         /* ticks held still before a lift */
#define CPAD_EDGE_LO  0.12f
#define CPAD_EDGE_HI  0.88f
#define SWIPE_TICKS   8

enum { CP_NONE = 0, CP_DOWN, CP_MOVE, CP_UP };

typedef struct {
    int   finger;           /* virtual finger is down */
    float x, y;             /* its position, 0..1 of the portrait screen */
    int   still;            /* ticks it has been held still */
    int   regrip;           /* lifting to re-grip, not because the stick is centred */
    int   swipe;            /* ticks left in a D-pad swipe, 0 = none */
    float swipe_dx, swipe_dy;   /* per-tick step of the swipe */
    int   swipe_still;      /* ticks held still at the swipe's end */
    unsigned prev_pad;      /* D-pad bits last tick */
} cpad_t;

typedef struct { int ev; float x, y; } cpad_out_t;

static float cpad_axis(int v)
{
    int a = v < 0 ? -v : v;
    if (a <= CPAD_DEAD) return 0.0f;
    if (a > CPAD_FULL) a = CPAD_FULL;
    float f = (float)(a - CPAD_DEAD) / (float)(CPAD_FULL - CPAD_DEAD);
    return v < 0 ? -f : f;
}

/* cx/cy: raw circle pad (right/up positive); valid: the ARM9 delivers it;
   pen: a real touch is down; dpad: bit0 left, bit1 right, bit2 up, bit3
   down (pressed; up/down come from the New 3DS C-stick), bit4 tap the
   status bar (New 3DS ZL).
   Returns the one event to send this tick in *o (CP_NONE = nothing). */
static void cpad_step(cpad_t *c, int cx, int cy, int valid, int pen, unsigned dpad, cpad_out_t *o)
{
    unsigned edge = dpad & ~c->prev_pad;
    c->prev_pad = dpad;
    o->ev = CP_NONE;
    if (pen) {                                  /* the real finger wins */
        if (c->finger) { c->finger = 0; c->swipe = 0; o->ev = CP_UP; o->x = c->x; o->y = c->y; }
        return;
    }
    if (c->swipe) {                             /* D-pad swipe in progress */
        if (--c->swipe == 0) { c->finger = 0; o->ev = CP_UP; }
        else if (c->swipe <= c->swipe_still) return;            /* held still: no fling */
        else { c->x += c->swipe_dx; c->y += c->swipe_dy; o->ev = CP_MOVE; }
        o->x = c->x; o->y = c->y;
        return;
    }
    float fx = valid ? cpad_axis(cx) : 0.0f, fy = valid ? cpad_axis(cy) : 0.0f;
    int tilted = fx != 0.0f || fy != 0.0f;
    if (!c->finger) {
        if (edge & 3) {                         /* start a page swipe */
            c->finger = 1;
            c->swipe = SWIPE_TICKS;
            c->x = (edge & 1) ? 0.15f : 0.85f;  /* left: finger moves right (previous page) */
            c->y = 0.5f;
            c->swipe_dx = ((edge & 1) ? 0.70f : -0.70f) / (SWIPE_TICKS - 1);
            c->swipe_dy = 0.0f;
            c->swipe_still = 0;
            o->ev = CP_DOWN; o->x = c->x; o->y = c->y;
            return;
        }
        if (edge & 16) {                        /* status-bar tap: down, held one tick, up */
            c->finger = 1;
            c->swipe_still = 1;
            c->swipe = 2;
            c->x = 0.5f;
            c->y = 0.012f;
            c->swipe_dx = c->swipe_dy = 0.0f;
            o->ev = CP_DOWN; o->x = c->x; o->y = c->y;
            return;
        }
        if (edge & 12) {                        /* scroll by about a page */
            c->finger = 1;
            c->swipe_still = CPAD_STILL;
            c->swipe = SWIPE_TICKS + CPAD_STILL;
            c->x = 0.5f;
            c->y = (edge & 4) ? 0.20f : 0.80f;  /* up: finger moves down (show what is above) */
            c->swipe_dx = 0.0f;
            c->swipe_dy = ((edge & 4) ? 0.60f : -0.60f) / (SWIPE_TICKS - 1);
            o->ev = CP_DOWN; o->x = c->x; o->y = c->y;
            return;
        }
        if (tilted) {
            c->finger = 1; c->still = 0; c->regrip = 0;
            c->x = 0.5f; c->y = 0.5f;
            o->ev = CP_DOWN; o->x = c->x; o->y = c->y;
        }
        return;
    }
    /* finger down: move, or hold still before lifting */
    float nx = c->x - fx * CPAD_SPEED * 1.5f;  /* x: the screen is 320 wide vs 480 */
    float ny = c->y + fy * CPAD_SPEED;
    int at_edge = nx < CPAD_EDGE_LO || nx > CPAD_EDGE_HI || ny < CPAD_EDGE_LO || ny > CPAD_EDGE_HI;
    if (tilted && !at_edge && !c->regrip) {
        c->x = nx; c->y = ny; c->still = 0;
        o->ev = CP_MOVE; o->x = c->x; o->y = c->y;
        return;
    }
    if (tilted && at_edge) c->regrip = 1;
    if (++c->still >= CPAD_STILL) {
        c->finger = 0; c->still = 0; c->regrip = 0;
        o->ev = CP_UP; o->x = c->x; o->y = c->y;
    }
}

#endif
