/* Host tests for the md0 write-back cache policy (boot/arm9/blkcache.c).
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "blkcache.h"

#define N 4
static u8 data[N * BLKC_SECTOR];
static u32 lba[N];
static u32 map[N * 2];
static u8 dirty[N];
static struct { u32 lba; u8 data[BLKC_SECTOR]; } writes[64];
static u32 nwrite, nattempt;
static int fail_at;

static int wb(u32 l, const u8 *d, void *ctx)
{
    (void)ctx;
    nattempt++;
    if (fail_at && nattempt == (u32)fail_at) return 5;
    assert(nwrite < 64);
    writes[nwrite].lba = l;
    memcpy(writes[nwrite].data, d, BLKC_SECTOR);
    nwrite++;
    return 0;
}

static void reset(blkcache_t *c)
{
    nwrite = 0;
    nattempt = 0;
    fail_at = 0;
    memset(writes, 0, sizeof(writes));
    blkc_init(c, data, lba, map, dirty, N);
}

int main(void)
{
    blkcache_t c;
    u8 buf[BLKC_SECTOR];
    reset(&c);
    for (u32 i = 0; i < BLKC_SECTOR; i++) buf[i] = (u8)(i * 3);
    assert(!blkc_get(&c, 10, buf));          /* miss */
    assert(blkc_put(&c, 10, buf, wb, NULL) == 0);
    assert(blkc_dirty_lba(&c, 10));
    u8 out[BLKC_SECTOR];
    assert(blkc_get(&c, 10, out));
    assert(memcmp(out, buf, BLKC_SECTOR) == 0);
    assert(blkc_flush(&c, wb, NULL) == 0);
    assert(nwrite == 1);
    assert(!blkc_get(&c, 10, out));          /* clean after flush */
    assert(blkc_dirty(&c) == 0);

    /* Eviction writes back the victim before its slot is reused. */
    reset(&c);
    for (u32 s = 0; s < N; s++) {
        memset(buf, 0x40 + s, sizeof buf);
        assert(blkc_put(&c, 100 + s, buf, wb, NULL) == 0);
    }
    memset(buf, 0xEE, sizeof buf);
    assert(blkc_put(&c, 200, buf, wb, NULL) == 0);
    assert(nwrite == 1 && writes[0].lba == 100);
    assert(writes[0].data[0] == 0x40);
    assert(blkc_get(&c, 101, out) && out[0] == 0x41);
    assert(!blkc_get(&c, 100, out));

    /* Flush walks each slot once; an error keeps that sector dirty. */
    reset(&c);
    for (u32 s = 0; s < 3; s++) {
        memset(buf, 0x50 + s, sizeof buf);
        assert(blkc_put(&c, 90 - s * 10, buf, wb, NULL) == 0);
    }
    fail_at = 2;
    assert(blkc_flush(&c, wb, NULL) == 5);
    assert(nwrite == 2 && writes[0].lba == 90 && writes[1].lba == 70);
    assert(blkc_dirty(&c) == 1 && blkc_dirty_lba(&c, 80));
    fail_at = 0;
    assert(blkc_flush(&c, wb, NULL) == 0);
    assert(nwrite == 3 && writes[2].lba == 80);
    assert(blkc_dirty(&c) == 0);

    /* Overwriting a cached sector coalesces into one writeback. */
    reset(&c);
    memset(buf, 1, sizeof buf);
    assert(blkc_put(&c, 5, buf, wb, NULL) == 0);
    memset(buf, 2, sizeof buf);
    assert(blkc_put(&c, 5, buf, wb, NULL) == 0);
    /* a write that bypassed the cache must not be resurrected by a flush */
    memset(buf, 3, sizeof buf);
    assert(blkc_put(&c, 6, buf, wb, NULL) == 0);
    blkc_invalidate_range(&c, 5, 1);
    assert(blkc_dirty(&c) == 1 && !blkc_dirty_lba(&c, 5));
    assert(blkc_flush(&c, wb, NULL) == 0);
    assert(nwrite == 1 && writes[0].lba == 6 && writes[0].data[0] == 3);
    printf("test_blkcache: PASS\n");
    return 0;
}
