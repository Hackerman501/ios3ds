/*
 * ios3ds shared definitions (ARM9 monitor <-> ARM11 loader <-> PC tools).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef unsigned int u32;      /* not uint32_t: newlib makes that "long", which breaks %x */
typedef uint64_t u64;
typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile unsigned int vu32;

#ifndef IOS3DS_VERSION
#define IOS3DS_VERSION "dev"
#endif

/* ---- fixed memory layout (see docs/m1-design.md) ---- */
#define ARM9_ENTRY          0x08000040u
#define ARM9_STACK_TOP      0x08100000u
#define ARM11_ENTRY         0x1FF90000u     /* AXI WRAM, below Luma's 0x1FFFF000 params */
#define SHARED_BASE         0x1FFF0000u     /* AXI WRAM mailbox (uncached on both CPUs) */
#define A11_RAM_VECTORS     0x1FFFFFA0u     /* boot11 exception vectors jump here (6 x 8 bytes) */
#define FCRAM_BASE          0x20000000u
#define FCRAM_SIZE          0x08000000u
#define FCRAM_N3DS_SIZE     0x10000000u      /* New 3DS: + the 128 MiB extension (docs/n3ds.md section 6) */
#define VRAM_BASE           0x18000000u
#define HID_PAD             (*(vu16 *)0x10146000u)   /* active low */

#define HID_A      (1u << 0)
#define HID_B      (1u << 1)
#define HID_SELECT (1u << 2)
#define HID_START  (1u << 3)
#define HID_RIGHT  (1u << 4)
#define HID_LEFT   (1u << 5)
#define HID_UP     (1u << 6)
#define HID_DOWN   (1u << 7)
#define HID_R      (1u << 8)
#define HID_L      (1u << 9)
#define HID_X      (1u << 10)
#define HID_Y      (1u << 11)

/* ---- ARM11 progress stages (shown by the ARM9 if the ARM11 stalls) ---- */
enum {
    A11_STAGE_NONE    = 0,
    A11_STAGE_ENTRY   = 1,
    A11_STAGE_IDREGS  = 2,
    A11_STAGE_PROBES  = 3,
    A11_STAGE_VFP     = 4,
    A11_STAGE_READY   = 5,
    A11_STAGE_JUMPING = 6,
    A11_STAGE_N3DS    = 7,
};

#define A9CMD_NONE  0u
#define A9CMD_JUMP  0x4A554D50u    /* 'JUMP' */
#define A9CMD_PROBE 0x50524F42u    /* 'PROB': run the instruction probes */
#define A9CMD_N3DS  0x5344334Eu    /* 'N3DS': model check, New 3DS clock / L2 (n3ds_req) */

/* New 3DS extras (docs/n3ds.md). ARM9 -> stub request bits (shared_t.n3ds_req) */
#define N3REQ_CLOCK  (1u << 0)     /* 804 MHz (boot.cfg n3ds_clock) */
#define N3REQ_L2     (1u << 1)     /* L2C-310 on, write-through (boot.cfg n3ds_l2) */
#define N3REQ_RAM    (1u << 2)     /* FCRAM extension for XNU (boot.cfg n3ds_ram) */
/* stub -> ARM9 result (shared_t.n3ds_res); the ARM9 copies it into the shim
   word SHIM_N3DS_OFF, which tells the shim how its timers run */
#define N3RES_DONE        (1u << 31)
#define N3RES_NEW         (1u << 16)  /* CFG11_SOCINFO bit 1 */
#define N3RES_804         (1u << 17)  /* PDN_LGR_SOCMODE = 804 MHz, acknowledged */
#define N3RES_L2          (1u << 18)  /* L2C-310 enabled (write-through) */
#define N3RES_TIMEOUT     (1u << 19)  /* clock switch not acknowledged: back in CTR mode */
#define N3RES_RAM         (1u << 20)  /* 0x28000000-0x2FFFFFFF is RAM of its own (probed): XNU gets 256 MiB */
#define N3RES_PRESCALE(r) ((r) & 0xFFu)          /* MPCore timer prescaler (0 = 127) */
#define N3RES_TBSHIFT(r)  (((r) >> 8) & 0xFu)    /* timer ticks = XNU ticks << shift */
#define SHIM_N3DS_OFF     0x0Cu       /* shim word (kernel/shim/entry.S cfg_n3ds) */

#define PROBE_MAX 10
enum { PROBE_NOT_RUN = 0, PROBE_OK = 1, PROBE_EXCEPTION = 2 };

typedef struct {
    u32 encoding;      /* the instruction word executed */
    u32 result;        /* PROBE_* */
    u32 value;         /* r0 after the instruction (0xDEADBEEF if it trapped) */
    u32 exc_slot;      /* boot11 RAM vector slot that fired (0..5) */
    u32 exc_mode;      /* CPSR mode at exception entry (0x1B = UND, 0x17 = ABT) */
} probe_t;

#define SHARED_MAGIC 0x53443349u   /* 'I3DS' */
#define SHARED_LOG_SIZE 4096u

typedef struct {
    u32 magic;
    u32 struct_size;
    /* ARM11 state */
    u32 a11_stage;
    u32 a11_heartbeat;
    u32 a11_current_probe;
    /* handshake: ARM9 writes a fresh token, ARM11's ready-loop echoes it.
       Guards against stale mailbox contents from a previous boot. */
    u32 a9_token;
    u32 a11_echo;
    /* ARM9 -> ARM11 command */
    u32 a9_cmd;
    u32 jump_pc;
    u32 jump_r0;
    u32 a11_ack;
    u32 probes_done;
    /* CPU identification (ARM11 core 0) */
    u32 midr, ctr, tlbtr, mpidr;
    u32 id_pfr0, id_pfr1, id_dfr0, id_afr0;
    u32 id_mmfr[4];
    u32 id_isar[6];
    u32 sctlr, actlr, cpacr, cpsr;
    u32 nprobes;
    probe_t probe[PROBE_MAX];
    u32 unexpected_count, unexpected_slot, unexpected_mode, unexpected_lr;
    /* New 3DS (A9CMD_N3DS): request, result, and the clock calibration
       handshake against the ARM9's fixed 65457 Hz timer (cal_state 1..3) */
    u32 n3ds_req, n3ds_res, n3ds_socinfo, n3ds_socmode, n3ds_lgr_cnt;
    u32 n3ds_periph_hz;     /* measured MPCore PERIPHCLK (timers), Hz */
    u32 n3ds_l2_aux, n3ds_l2_ctrl, n3ds_l2_id;
    u32 cal_state, cal_a11;
    /* ARM11 -> ARM9 text log ring (single producer = ARM11) */
    u32 log_head;
    char log_buf[SHARED_LOG_SIZE];
} shared_t;

#define SHARED ((volatile shared_t *)SHARED_BASE)

/*
 * XNU-era log ring (M2). Lives in FCRAM below topOfKernelData; filled by the
 * kernel console hook, drained by the ARM9 monitor.
 */
#define XNU_RING_MAGIC 0x474E4952u  /* 'RING' */
typedef struct {
    u32 magic;
    u32 size;          /* bytes in buf */
    u32 head;          /* total bytes ever written (producer) */
    u32 reserved;
    char buf[];
} xnu_ring_t;

/* ---- XNU boot_args (verified layout, docs/research.md 2.2) ---- */
typedef struct {
    u32 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
} boot_video_t;

typedef struct {
    u16 Revision;
    u16 Version;
    u32 virtBase;
    u32 physBase;
    u32 memSize;
    u32 topOfKernelData;
    boot_video_t Video;
    u32 machineType;
    u32 deviceTreeP;        /* kernel VIRTUAL address */
    u32 deviceTreeLength;
    char CommandLine[256];
} boot_args_t;

_Static_assert(sizeof(boot_args_t) == 0x138, "boot_args layout");
_Static_assert(offsetof(boot_args_t, deviceTreeP) == 0x30, "boot_args layout");

/* ---- kernel shim (kernel/shim), fixed placement inside XNU's RAM ---- */
#define SHIM_PA          0x20840000u
/* Older kernelcaches are linked at 0xC0000000; their shim needs the matching
   VA. The default is 4.2.1's, and only the shim build passes an override. */
#ifndef IOS3DS_SHIM_VA
#define IOS3DS_SHIM_VA   0x80840000u
#endif
#define SHIM_VA          IOS3DS_SHIM_VA
#define SHIM_SIZE        0x10000u
#define SHIM_PANIC_REQ   0x6500u        /* offset: ARM9 writes 1 -> XNU panics with a thread dump; a
                                            cache line only the ARM9 writes (kernel/shim/entry.S) */
#define SHIM_BLK_OFF     0x6000u        /* blkmbox_t */
#define SHIM_BOUNCE_OFF  0x8000u        /* bounce buffer for block I/O */
#define SHIM_BOUNCE_SIZE 0x8000u
#define SHIM_RING_PA     (SHIM_PA + SHIM_SIZE)

/*
 * Block mailbox: XNU's md0 (memdev) strategy routine -> ARM9 -> SD file.
 * Each 32-byte cache line has a single writer, so the ARM11 can clean or
 * invalidate lines without clobbering the other side's data.
 */
#define BLK_MAGIC  0x4D4B4C42u   /* 'BLKM' */
#define BLK_READ   1u
#define BLK_WRITE  2u
typedef struct {
    /* line 0: ARM9 */
    u32 magic;          /* BLK_MAGIC once the ARM9 serves requests */
    u32 dev_sectors;    /* backing file size / 512 */
    u32 _r0[6];
    /* line 1: ARM11 */
    u32 seq;            /* request number; a new value = new request */
    u32 op;
    u32 lba;            /* 512-byte sectors */
    u32 count;          /* sectors, <= SHIM_BOUNCE_SIZE / 512 */
    u32 xnu_bytes;      /* size of the whole XNU buf this chunk belongs to (profiling) */
    u32 xnu_flags;      /* its buf_flags(): B_READ 1, B_ASYNC 2, B_PAGEIO 0x80, B_META 0x100;
                           bit 31 = first chunk of the buf */
    u32 _r1[2];
    /* line 2: ARM9 */
    u32 done_seq;       /* == seq when the request has completed */
    int status;         /* 0 = ok, else FatFs error */
    u32 _r2[6];
    /* line 3: ARM11 statistics */
    u32 nreq, nerr, nread, nwrite;
    u32 _r3[4];
} blkmbox_t;
_Static_assert(sizeof(blkmbox_t) == 128, "blkmbox layout");
