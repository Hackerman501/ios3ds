/*
 * ios3ds shim: export the ARM9's input mailbox to userland as the read-only
 * sysctl "kern.ios3ds_input" (512 opaque bytes = inmbox_t).
 *
 * NOT wired in yet. To wire: copy to kernel/shim/input.c, add it to the gcc
 * command in scripts/mkboot.py build_shim() (next to entry.S and blk.c), and
 * call shim_input_register() once from thread context - the first line of
 * shim_mdevstrategy() in blk.c is the natural place (first call = root
 * mount in bsd_init, long before launchd). No new
 * kernel patch is needed: sysctl_register_oid() is a normal KPI and the OID
 * lives in shim memory, which XNU never reuses.
 *
 * Kernel facts (iPod2,1 4.2.1 8C148, xnu-1504.58.28; addresses from the
 * kernelcache symbol table, layouts read out of __DATA):
 *   sysctl_register_oid    0x80159c54 (Thumb)  takes the funnel + the sysctl
 *                          geometry rw-lock (allocating it if needed);
 *                          oid_number -1 (OID_AUTO) becomes max(100, last)+1
 *   sysctl_handle_opaque   0x80159242 (Thumb)  = sysctl_io_opaque(req, arg1, arg2, NULL)
 *   sysctl__kern_children  0x8027f7b8          list head for "kern."
 *   struct sysctl_oid is the 36-byte pre-OID2 layout (no descr/version), e.g.
 *   kern.nbuf at 0x80257380 = { 0x8027f7b8, 0, -1, 0x80000002, &nbuf, 0,
 *   "nbuf", sysctl_handle_int|1, "I" }.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "../input_proto.h"     /* -> boot/common/ios3ds.h once wired */

struct sysctl_oid {
    void       *oid_parent;     /* struct sysctl_oid_list * */
    void       *oid_link;       /* SLIST_ENTRY */
    int         oid_number;
    int         oid_kind;
    void       *oid_arg1;
    int         oid_arg2;
    const char *oid_name;
    int       (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};
_Static_assert(sizeof(struct sysctl_oid) == 36, "xnu-1504 arm sysctl_oid");

#define CTLFLAG_RD      0x80000000
#define CTLTYPE_OPAQUE  5
#define OID_AUTO        (-1)

#define KPI(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define sysctl_register_oid   KPI(0x80159c54, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPI(0x80159242, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_KERN_CHILDREN  ((void *)0x8027f7b8)

#define INMBOX ((volatile inmbox_t *)(SHIM_VA + SHIM_INPUT_OFF))

/* The ARM9 writes the mailbox behind the ARM11's back (FCRAM is uncached on
   the ARM9): drop our cached copy of those lines before every read. Nothing on
   the ARM11 ever writes them, so invalidating cannot lose data. */
static void input_dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static int input_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    input_dinval(arg1, (u32)arg2);
    /* copyout straight from the mailbox: the reader re-checks `head`
       (inmbox_t is written sample-first, head-last), no kernel-side copy */
    return sysctl_handle_opaque(oidp, arg1, arg2, req);
}

static struct sysctl_oid input_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_OPAQUE),
    .oid_arg1    = (void *)(SHIM_VA + SHIM_INPUT_OFF),
    .oid_arg2    = (int)sizeof(inmbox_t),
    .oid_name    = "ios3ds_input",
    .oid_handler = input_sysctl,
    .oid_fmt     = "S,ios3ds_input",
};

/* Thread context only (takes the funnel and a rw-lock). Idempotent. */
void shim_input_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&input_oid);
}
