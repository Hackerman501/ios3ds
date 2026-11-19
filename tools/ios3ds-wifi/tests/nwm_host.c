/*
 * Host driver for boot/arm9/wifi/nwmcode.c (the ARM9's in-place NWM .code
 * extractor): nwm_host <nwm.code> <out.bin> [1|4|5]. test_nwmfw.py checks that
 * its container equals tools/ios3ds-wifi/nwmfw.py's byte for byte.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include "wifi.h"

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    u32 cap = 4u << 20;
    u8 *buf = calloc(1, cap), *out = calloc(1, 64 * 1024);
    u32 len = (u32)fread(buf, 1, cap, f), outlen = 0;
    fclose(f);
    nwm_info_t in;
    int r = nwm_extract(buf, len, cap, argc > 3 ? (u32)atoi(argv[3]) : 4, out, 64 * 1024, &outlen, &in);
    printf("r=%d layout=%u blz=%u base=%x t1=%x t4=%x t5=%x dbstub=%u\n", r, in.layout, in.blz,
           in.text_base, in.main_size[0], in.main_size[1], in.main_size[2], in.db_end_is_stub);
    if (!r && (f = fopen(argv[2], "wb"))) {
        fwrite(out, 1, outlen, f);
        fclose(f);
    } else {
        printf("%s\n", nwm_strerror(r));
    }
    free(buf);
    free(out);
    return r ? 1 : 0;
}
