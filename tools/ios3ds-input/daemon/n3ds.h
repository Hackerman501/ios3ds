/* New 3DS extras -> iOS (docs/n3ds.md section 3, docs/input-design.md 9).
 *
 * C-stick: a flick past CS_ON (of about +-127) in one direction is one
 * "D-pad" press for cpad_step(): left/right = previous/next page, up/down =
 * scroll by about a page (cpad.h). Only the dominant axis counts; the stick
 * must come back inside CS_OFF before the next flick (hysteresis), so a held
 * stick gives exactly one gesture.
 * ZL: scroll to the top = a tap on the status bar (cpad.h dpad bit 4). Not
 *     the app switcher: the iPod touch 2G has no multitasking in iOS 4, and
 *     two HOME presses did nothing more than one in QEMU.
 * Each key carries the pause before it (ms), so the presses look human.
 * ZR: a screenshot to the Camera Roll = POWER pressed while HOME is held
 *     (the white flash shows in QEMU).
 *
 * Pure logic, host-tested by tools/ios3ds-input/tests/test_n3ds.c. */
#ifndef IOS3DS_N3DS_H
#define IOS3DS_N3DS_H

#define CS_ON    64
#define CS_OFF   32

enum { N3K_MENU = 1, N3K_POWER = 2 };
typedef struct { unsigned char key, down; unsigned short wait_ms; } n3key_t;   /* wait before sending */

typedef struct { unsigned dir; } cstick_t;      /* latched direction bit, 0 = centred */

/* x right / y up positive (inmbox ext). Returns the cpad_step() dpad bits:
   bit0 left, bit1 right, bit2 up, bit3 down. */
static unsigned cstick_dirs(cstick_t *s, int x, int y, int valid)
{
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (!valid) { s->dir = 0; return 0; }
    if (s->dir) {
        if (ax < CS_OFF && ay < CS_OFF) s->dir = 0;
        return s->dir;
    }
    if (ax >= CS_ON && ax >= ay) s->dir = x < 0 ? 1u : 2u;
    else if (ay >= CS_ON) s->dir = y > 0 ? 4u : 8u;
    return s->dir;
}

/* key sequence for a ZR press (went down this sample); returns the number
   of entries written to out[4]. ZL is a touch, not keys: cpad_step's dpad
   bit 4 (status-bar tap). */
static int zr_keys(int zr, n3key_t out[4])
{
    static const n3key_t sh[4] = { {N3K_MENU, 1, 0}, {N3K_POWER, 1, 150}, {N3K_POWER, 0, 100}, {N3K_MENU, 0, 100} };
    if (!zr) return 0;
    for (int i = 0; i < 4; i++) out[i] = sh[i];
    return 4;
}

#endif
