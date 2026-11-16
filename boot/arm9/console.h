/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

#define CON_TOP    0
#define CON_BOTTOM 1

#define CON_BG     0x000000
#define CON_WHITE  0xE0E0E0
#define CON_GREEN  0x40FF40
#define CON_YELLOW 0xFFE040
#define CON_RED    0xFF4040
#define CON_CYAN   0x40E0FF
#define CON_GREY   0x909090

void con_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1);
bool con_available(void);
void con_set_color(int which, u32 color);
void con_putc(int which, char c);
void con_puts(int which, const char *s);
void con_line(int which, int row, u32 color, const char *s);
