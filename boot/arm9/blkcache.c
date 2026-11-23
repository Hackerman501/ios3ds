/*
 * Write-back sector cache (docs/boot-perf.md). Kept separate from FatFs so the
 * replacement/flush policy can be unit-tested on the host.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "blkcache.h"

void blkc_init(blkcache_t *c, u8 *data, u32 *lba, u32 *map, u8 *dirty, u32 slots)
{
    c->slots = slots;
    c->next = 0;
    c->data = data;
    c->lba = lba;
    c->map = map;
    c->dirty = dirty;
    c->hits = c->misses = c->evictions = c->written = c->direct_writes = c->flushes = 0;
    for (u32 i = 0; i < slots * 2; i++) map[i] = 0;
    for (u32 i = 0; i < slots; i++) {
        c->lba[i] = ~0u;
        c->dirty[i] = 0;
    }
}

/* 2 x slots entries, never full: at most slots of them are occupied. */
static int find(const blkcache_t *c, u32 lba)
{
    u32 mask = c->slots * 2 - 1, h = (lba * 2654435761u) >> 23 & mask;
    for (u32 n = 0; n < c->slots * 2; n++, h = (h + 1) & mask) {
        u32 v = c->map[h];
        if (!v) return -1;
        if (c->lba[v - 1] == lba) return (int)(v - 1);
    }
    return -1;
}

static void map_put(blkcache_t *c, u32 lba, u32 slot)
{
    u32 mask = c->slots * 2 - 1, h = (lba * 2654435761u) >> 23 & mask;
    while (c->map[h]) h = (h + 1) & mask;
    c->map[h] = slot + 1;
}

static void map_rehash(blkcache_t *c)
{
    for (u32 i = 0; i < c->slots * 2; i++) c->map[i] = 0;
    for (u32 i = 0; i < c->slots; i++)
        if (c->lba[i] != ~0u) map_put(c, c->lba[i], i);
}

bool blkc_dirty_lba(const blkcache_t *c, u32 lba)
{
    int i = find(c, lba);
    return i >= 0 && c->dirty[i];
}

bool blkc_get(const blkcache_t *c, u32 lba, u8 *dst)
{
    int i = find(c, lba);
    if (i < 0 || !c->dirty[i]) return false;
    memcpy(dst, c->data + (u32)i * BLKC_SECTOR, BLKC_SECTOR);
    return true;
}

int blkc_put(blkcache_t *c, u32 lba, const u8 *src, blkc_write_fn wb, void *ctx)
{
    int slot = find(c, lba);
    if (slot < 0) {
        slot = (int)c->next;
        c->next = (c->next + 1) % c->slots;
        if (c->dirty[slot]) {
            u32 old = c->lba[slot];
            int r = wb(old, c->data + (u32)slot * BLKC_SECTOR, ctx);
            if (r) return r;            /* keep the old sector dirty: no data loss */
            c->evictions++;
            c->written++;
            c->dirty[slot] = 0;
        }
        if (c->lba[slot] != ~0u) {
            c->lba[slot] = ~0u;
            map_rehash(c);
        }
        c->lba[slot] = lba;
        map_put(c, lba, (u32)slot);
    } else {
        c->hits++;
    }
    memcpy(c->data + (u32)slot * BLKC_SECTOR, src, BLKC_SECTOR);
    c->dirty[slot] = 1;
    return 0;
}

int blkc_flush(blkcache_t *c, blkc_write_fn wb, void *ctx)
{
    int first_err = 0;
    /* One linear pass. An earlier version sorted dirty slots by LBA with an
       O(slots^2) scan per flush; QEMU showed 2143 flushes in 29 guest seconds
       and the sorting alone cost more than the SD writes it saved. Slots are
       allocated round-robin, so their order is already close to write order. */
    for (u32 i = 0; i < c->slots; i++) {
        if (!c->dirty[i]) continue;
        int r = wb(c->lba[i], c->data + i * BLKC_SECTOR, ctx);
        if (r) {
            if (!first_err) first_err = r;
            continue;                   /* stay dirty */
        }
        c->dirty[i] = 0;
        c->written++;
    }
    if (!first_err) c->flushes++;
    return first_err;
}

void blkc_invalidate_range(blkcache_t *c, u32 lba, u32 count)
{
    bool changed = false;
    for (u32 i = 0; i < c->slots; i++) {
        if (c->dirty[i] && c->lba[i] >= lba && c->lba[i] - lba < count) {
            /* A direct write already contains this data; nothing is lost by
               dropping the overlay (the caller syncs the file later). */
            c->dirty[i] = 0;
            c->lba[i] = ~0u;
            changed = true;
        }
    }
    if (changed) map_rehash(c);
}

u32 blkc_dirty(const blkcache_t *c)
{
    u32 n = 0;
    for (u32 i = 0; i < c->slots; i++) n += c->dirty[i] != 0;
    return n;
}
