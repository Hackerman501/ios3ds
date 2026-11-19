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
    bool hidden;        /* iOS owns this screen: keep the text, draw nothing */
    char text[30][50];
    u32 tcolor[30];
    u32 dirty;          /* deferred mode: rows still to paint (bit r) */
} con_t;

static con_t cons[2];
static bool have_fb;
/* Deferred painting (con_defer): text only goes into the buffers and the
   monitor loop paints a few rows per pass (con_paint). Hardware run 17: with
   the log on a visible screen every scrolled line repainted the whole
   console (~30k pixels per row, byte writes into VRAM) inside the log call,
   and a busy log brought the ARM9 loop down to 1 pass/s: buttons and the
   touch screen were sampled that rarely. */
static bool deferred;

static void draw_char(con_t *c, int col, int row, char ch, u32 fg)
{
    if (c->hidden) return;
    const unsigned char *g = font8x8_basic[(unsigned char)ch & 0x7F];
    int x0 = col * 8, y0 = row * 8;
    u8 fb0 = fg & 0xFF, fg1 = (fg >> 8) & 0xFF, fg2 = (fg >> 16) & 0xFF;
    u8 bg0 = CON_BG & 0xFF, bg1 = (CON_BG >> 8) & 0xFF, bg2 = (CON_BG >> 16) & 0xFF;
    for (int i = 0; i < 2; i++) {
        if (!c->fb[i]) continue;
        for (int x = 0; x < 8; x++) {
            /* screen column x0 + x; screen row y0 + y is at SCR_H - 1 - (y0 + y) */
            u8 *p = c->fb[i] + ((size_t)(x0 + x) * SCR_H + (SCR_H - 1 - y0)) * BPP;
            for (int y = 0; y < 8; y++, p -= BPP) {
                bool on = (g[y] >> x) & 1;
                p[0] = on ? fb0 : bg0;
                p[1] = on ? fg1 : bg1;
                p[2] = on ? fg2 : bg2;
            }
        }
    }
}

static void draw_row(con_t *c, int r)
{
    for (int col = 0; col < c->cols; col++)
        draw_char(c, col, r, c->text[r][col], c->tcolor[r]);
}

static void redraw(con_t *c)
{
    for (int r = 0; r < c->rows; r++)
        draw_row(c, r);
    c->dirty = 0;
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
        if (deferred) c->dirty = (1u << c->rows) - 1;
        else if (have_fb) redraw(c);
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
    if (deferred) c->dirty |= 1u << c->cy;
    else if (have_fb) draw_char(c, c->cx, c->cy, ch, c->color);
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
    if (deferred) c->dirty |= 1u << row;
    else if (have_fb) draw_row(c, row);
}

void con_defer(bool on)
{
    deferred = on;
    if (!on)
        for (int i = 0; i < 2; i++)
            while (cons[i].dirty) con_paint(30);
}

/* paint at most max_rows of the rows changed since the last call */
void con_paint(int max_rows)
{
    for (int i = 0; i < 2 && max_rows > 0; i++) {
        con_t *c = &cons[i];
        if (!c->dirty) continue;
        if (!have_fb || c->hidden) {        /* con_show repaints everything when shown */
            c->dirty = 0;
            continue;
        }
        for (int r = 0; r < c->rows && max_rows > 0; r++)
            if (c->dirty & (1u << r)) {
                c->dirty &= ~(1u << r);
                draw_row(c, r);
                max_rows--;
            }
    }
}

/* hide a console while iOS is shown on that screen; showing it again repaints
   the text kept meanwhile */
void con_show(int which, bool on)
{
    con_t *c = &cons[which];
    if (c->hidden == !on) return;
    c->hidden = !on;
    if (on && have_fb) {
        for (int i = 0; i < 2; i++)
            if (c->fb[i]) memset(c->fb[i], 0, (size_t)c->width * SCR_H * BPP);
        redraw(c);
    }
}
