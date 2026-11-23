/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "util.h"

/* Write-back sector cache for md0, used by boot/arm9/blksrv.c. Only sectors
   XNU has written are cached: reads go through unless the sector has a dirty
   overlay. Dirty sectors are written back on eviction, on an idle flush or
   before we set the HFS+ clean-unmount flag, so a clean shutdown is durable
   and a hard cut leaves the volume marked dirty.
   No FatFs/ARM9 dependencies here: tests/test_blkcache.c compiles this on host. */

#define BLKC_SECTOR 512u

typedef int (*blkc_write_fn)(u32 lba, const u8 *data, void *ctx);

typedef struct {
    u32 slots;
    u32 next;               /* round-robin replacement hand */
    u8 *data;               /* slots * BLKC_SECTOR */
    u32 *lba;               /* ~0u = unused */
    u32 *map;               /* 2 * slots open-addressing index, slot+1 or 0 */
    u8 *dirty;
    u32 hits, misses, evictions, written, direct_writes, flushes;
    u32 ndirty;             /* dirty slots: blkc_dirty() and the read fast path are O(1) */
} blkcache_t;

void blkc_init(blkcache_t *c, u8 *data, u32 *lba, u32 *map, u8 *dirty, u32 slots);
/* true + dst filled: this sector has a dirty overlay. */
bool blkc_get(const blkcache_t *c, u32 lba, u8 *dst);
/* cheap overlay test for a read run */
bool blkc_dirty_lba(const blkcache_t *c, u32 lba);
/* Cache one sector; evicts (and writes back) a victim if needed.
   Returns 0 or the writeback callback's error. */
int blkc_put(blkcache_t *c, u32 lba, const u8 *src, blkc_write_fn wb, void *ctx);
/* Write back every dirty sector (one linear pass, slot order). Returns 0 or
   the first callback error; sectors that failed stay dirty for the next flush. */
int blkc_flush(blkcache_t *c, blkc_write_fn wb, void *ctx);
/* Drop overlays superseded by a write that bypasses the cache. */
void blkc_invalidate_range(blkcache_t *c, u32 lba, u32 count);
u32 blkc_dirty(const blkcache_t *c);
