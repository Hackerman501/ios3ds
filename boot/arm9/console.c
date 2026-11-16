/*
 * Text consoles on the two LCDs, drawn by the ARM9 straight into the
 * framebuffers the loader (Luma) set up. Framebuffers are BGR8 and stored
 * rotated: memory column x = screen column x, pixels bottom-to-top.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console.h"
#include "util.h"
#include "font8x8_basic.h"

#define SCR_H 240
#define BPP 3

typedef struct {
    u8 *fb[2];          /* draw into both buffer sets; whichever is shown works */
    int width;          /* pixels */
    int cols, rows;
    int cx, cy;
    u32 color;
    char text[30][50];
    u32 tcolor[30];
} con_t;

static con_t cons[2];
static bool have_fb;

static void put_px_col(con_t *c, int x, int y, u32 rgb)
{
    for (int i = 0; i < 2; i++) {
        if (!c->fb[i]) continue;
        u8 *p = c->fb[i] + ((size_t)x * SCR_H + (SCR_H - 1 - y)) * BPP;
        p[0] = rgb & 0xFF;           /* B */
        p[1] = (rgb >> 8) & 0xFF;    /* G */
        p[2] = (rgb >> 16) & 0xFF;   /* R */
    }
}

static void draw_char(con_t *c, int col, int row, char ch, u32 fg)
{
    const unsigned char *g = font8x8_basic[(unsigned char)ch & 0x7F];
    int x0 = col * 8, y0 = row * 8;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            put_px_col(c, x0 + x, y0 + y, (g[y] >> x) & 1 ? fg : CON_BG);
        }
    }
}

static void redraw(con_t *c)
{
    for (int r = 0; r < c->rows; r++) {
        for (int col = 0; col < c->cols; col++) {
            draw_char(c, col, r, c->text[r][col], c->tcolor[r]);
        }
    }
}

static void clear(con_t *c)
{
    for (int i = 0; i < 2; i++) {
        if (c->fb[i]) memset(c->fb[i], 0, (size_t)c->width * SCR_H * BPP);
    }
    memset(c->text, ' ', sizeof(c->text));
    for (int r = 0; r < 30; r++) c->tcolor[r] = CON_WHITE;
    c->cx = c->cy = 0;
}

void con_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1)
{
    cons[CON_TOP].fb[0] = top0;
    cons[CON_TOP].fb[1] = (top1 != top0) ? top1 : NULL;
    cons[CON_TOP].width = 400;
    cons[CON_TOP].cols = 50;
    cons[CON_BOTTOM].fb[0] = bot0;
    cons[CON_BOTTOM].fb[1] = (bot1 != bot0) ? bot1 : NULL;
    cons[CON_BOTTOM].width = 320;
    cons[CON_BOTTOM].cols = 40;
    for (int i = 0; i < 2; i++) {
        cons[i].rows = 30;
        cons[i].color = CON_WHITE;
        clear(&cons[i]);
    }
    have_fb = top0 || bot0;
}

bool con_available(void) { return have_fb; }

void con_set_color(int which, u32 color) { cons[which].color = color; }

static void newline(con_t *c)
{
    c->cx = 0;
    if (++c->cy >= c->rows) {
        memmove(c->text[0], c->text[1], sizeof(c->text[0]) * (c->rows - 1));
        memmove(&c->tcolor[0], &c->tcolor[1], sizeof(u32) * (c->rows - 1));
        memset(c->text[c->rows - 1], ' ', sizeof(c->text[0]));
        c->cy = c->rows - 1;
        c->tcolor[c->cy] = c->color;
        if (have_fb) redraw(c);
    } else {
        c->tcolor[c->cy] = c->color;
        memset(c->text[c->cy], ' ', sizeof(c->text[0]));
    }
}

void con_putc(int which, char ch)
{
    con_t *c = &cons[which];
    if (ch == '\r') return;
    if (ch == '\n') { newline(c); return; }
    if (ch == '\t') ch = ' ';
    if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
    if (c->cx >= c->cols) newline(c);
    c->text[c->cy][c->cx] = ch;
    c->tcolor[c->cy] = c->color;
    if (have_fb) draw_char(c, c->cx, c->cy, ch, c->color);
    c->cx++;
}

void con_puts(int which, const char *s)
{
    while (*s) con_putc(which, *s++);
}

/* write a fixed line (status display), no scrolling */
void con_line(int which, int row, u32 color, const char *s)
{
    con_t *c = &cons[which];
    if (row < 0 || row >= c->rows) return;
    memset(c->text[row], ' ', sizeof(c->text[0]));
    for (int i = 0; i < c->cols && s[i]; i++) c->text[row][i] = s[i];
    c->tcolor[row] = color;
    if (have_fb) {
        for (int col = 0; col < c->cols; col++) draw_char(c, col, row, c->text[row][col], color);
    }
}
