/*
 * ios3ds: minimal Old Nintendo 3DS ("CTR") machine for QEMU.
 *
 * This is a development model for the ios3ds project, NOT a 3DS emulator.
 * It models just enough of the console for a Luma3DS-chainloaded .firm payload
 * that runs bare metal on both the ARM9 and the ARM11:
 *
 *   - ARM946E-S  ("arm946")      with its own address space (ITCM, ARM9 RAM,
 *                                 ARM9-only IO, DTCM, boot9 vector ROM)
 *   - ARM11 MPCore core 0 ("arm11mpcore"); core 1 is "parked" (not created)
 *   - MPCore private region @0x17E00000 (SCU, GIC, private timer, watchdog)
 *   - FCRAM 128 MiB @0x20000000, VRAM 6 MiB @0x18000000, AXI WRAM 512 KiB
 *     @0x1FF80000, DSP RAM @0x1FF00000, shared between both CPUs
 *   - TMIO SD host controller @0x10006000 (ARM9) backed by QEMU's SD card
 *   - PDC0/PDC1 framebuffer registers @0x10400400/500 + a 400x480 display
 *     (top screen above bottom screen), rotated framebuffers like hardware
 *   - HID pad register @0x10146000 (keyboard mapped)
 *   - CFG11 SOCINFO (reports Old 3DS)
 *
 * Boot: "-kernel payload.firm" is loaded the way Luma3DS's chainloader does it
 * (sections copied, ARM9 entry with argc=2/argv incl. framebuffer pointers when
 * the FIRM requests screen init, ARM11 entry with MMU/caches off).
 *
 * Sources for every address used here are in docs/research.md (libn3ds
 * mem_map.h, Luma3DS chainloader, linux-3ds DTS).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/arm/machines-qom.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sd.h"
#include "hw/cpu/arm11mpcore.h"
#include "hw/cpu/cluster.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"

/* ------------------------------------------------------------------------ */
/* Memory map                                                               */

#define CTR_ITCM_BASE        0x00000000
#define CTR_ITCM_SIZE        (32 * KiB)
#define CTR_ARM9RAM_BASE     0x08000000
#define CTR_ARM9RAM_SIZE     (1 * MiB)
#define CTR_IO9_BASE         0x10000000
#define CTR_CFG9_BASE        0x10000000
#define CTR_TMIO1_BASE       0x10006000
#define CTR_IO_SHARED_BASE   0x10100000
#define CTR_CFG11_BASE       0x10140000
#define CTR_HID_BASE         0x10146000
#define CTR_IO11_BASE        0x10200000
#define CTR_PDC0_BASE        0x10400400
#define CTR_PDC1_BASE        0x10400500
#define CTR_MPCORE_BASE      0x17E00000
#define CTR_VRAM_BASE        0x18000000
#define CTR_VRAM_SIZE        (6 * MiB)
#define CTR_DSPRAM_BASE      0x1FF00000
#define CTR_DSPRAM_SIZE      (512 * KiB)
#define CTR_AXIWRAM_BASE     0x1FF80000
#define CTR_AXIWRAM_SIZE     (512 * KiB)
#define CTR_FCRAM_BASE       0x20000000
#define CTR_FCRAM_SIZE       (128 * MiB)
#define CTR_DTCM_BASE        0xFFF00000
#define CTR_DTCM_SIZE        (16 * KiB)
#define CTR_BOOTROM_HI       0xFFFF0000
#define CTR_BOOTROM_SIZE     (64 * KiB)

/* boot11 RAM vector table the ROM's exception vectors branch to */
#define CTR_A11_RAMVEC       0x1FFFFFA0
/* ARM9 exception veneers boot9 jumps to (Luma installs 6 x 8 bytes here) */
#define CTR_A9_RAMVEC        0x08000000

#define CTR_TOP_W   400
#define CTR_BOT_W   320
#define CTR_SCR_H   240

/* ------------------------------------------------------------------------ */
/* TMIO SD host controller (subset used by Normmatt's sdmmc.c and libn3ds)  */

#define TYPE_CTR_TMIO "ctr-tmio"
OBJECT_DECLARE_SIMPLE_TYPE(CtrTmioState, CTR_TMIO)

#define TMIO_CMD        0x00
#define TMIO_PORTSEL    0x02
#define TMIO_ARG0       0x04
#define TMIO_ARG1       0x06
#define TMIO_STOP       0x08
#define TMIO_BLKCOUNT   0x0A
#define TMIO_RESP0      0x0C   /* .. 0x1A */
#define TMIO_STAT0      0x1C
#define TMIO_STAT1      0x1E
#define TMIO_MASK0      0x20
#define TMIO_MASK1      0x22
#define TMIO_CLKCTL     0x24
#define TMIO_BLKLEN     0x26
#define TMIO_OPT        0x28
#define TMIO_ERRSTAT0   0x2C
#define TMIO_ERRSTAT1   0x2E
#define TMIO_FIFO16     0x30
#define TMIO_DATACTL    0xD8
#define TMIO_RESET      0xE0
#define TMIO_DATACTL32  0x100
#define TMIO_BLKLEN32   0x104
#define TMIO_BLKCOUNT32 0x108
#define TMIO_FIFO32     0x10C

#define STAT0_CMDRESPEND  0x0001
#define STAT0_DATAEND     0x0004
#define STAT0_CARD_INSERT 0x0010
#define STAT0_SIGSTATE    0x0020
#define STAT0_WRPROTECT   0x0080  /* set = writable (linux tmio_mmc get_ro) */
#define STAT1_CMDTIMEOUT  0x0040
#define STAT1_RXRDY       0x0100
#define STAT1_TXRQ        0x0200
#define STAT1_CMD_BUSY    0x4000

#define CTL32_RX32RDY     0x0100
#define CTL32_TX32RQ      0x0200

struct CtrTmioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;

    uint16_t regs[0x200 / 2];
    uint16_t stat0, stat1;
    uint16_t resp[8];

    /* data phase */
    bool     xfer_read, xfer_write;
    uint32_t blocks_left;
    uint32_t blklen;
    uint8_t  block[512];
    uint32_t pos;          /* byte position inside block */
    bool     auto_stop;
};

static uint16_t tmio_live_stat0(CtrTmioState *s)
{
    uint16_t v = s->stat0;
    if (sdbus_get_inserted(&s->sdbus)) {
        v |= STAT0_SIGSTATE | STAT0_CARD_INSERT;
        if (!sdbus_get_readonly(&s->sdbus)) {
            v |= STAT0_WRPROTECT;
        }
    }
    return v;
}

static void tmio_finish_data(CtrTmioState *s)
{
    s->xfer_read = s->xfer_write = false;
    s->stat0 |= STAT0_DATAEND;
    s->regs[TMIO_DATACTL32 / 2] &= ~(CTL32_RX32RDY);
    if (s->auto_stop) {
        SDRequest rq = { .cmd = 12, .arg = 0 };
        uint8_t r[16];
        sdbus_do_command(&s->sdbus, &rq, r, sizeof(r));
    }
}

static void tmio_load_read_block(CtrTmioState *s)
{
    if (!s->blocks_left) {
        tmio_finish_data(s);
        return;
    }
    sdbus_read_data(&s->sdbus, s->block, s->blklen);
    s->pos = 0;
    s->regs[TMIO_DATACTL32 / 2] |= CTL32_RX32RDY;
    s->stat1 |= STAT1_RXRDY;
}

static void tmio_do_command(CtrTmioState *s, uint16_t cmdreg)
{
    SDRequest rq;
    uint8_t r[16];
    size_t rlen;
    unsigned rtype = (cmdreg >> 8) & 7;     /* 0=auto 3=none 4=R1/R6/R7 5=R1b 6=R2 7=R3 */
    bool data = cmdreg & 0x0800;
    bool read = cmdreg & 0x1000;
    bool multi = cmdreg & 0x2000;

    rq.cmd = cmdreg & 0x3F;
    rq.arg = s->regs[TMIO_ARG0 / 2] | ((uint32_t)s->regs[TMIO_ARG1 / 2] << 16);
    rq.crc = 0;
    rlen = sdbus_do_command(&s->sdbus, &rq, r, sizeof(r));

    memset(s->resp, 0, sizeof(s->resp));
    if (rtype >= 4 || (rtype == 0 && rq.cmd != 0)) {
        if (rlen == 0) {
            s->stat1 |= STAT1_CMDTIMEOUT;
            qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: CMD%d timeout\n", rq.cmd);
            return;
        }
        if (rlen == 4) {
            uint32_t v = ldl_be_p(r);
            s->resp[0] = v;
            s->resp[1] = v >> 16;
        } else if (rlen == 16) {
            /* TMIO stores R2 bits [127:8] (CRC dropped) as a LE 128-bit value */
            uint8_t le[16] = { 0 };
            for (int i = 0; i < 15; i++) {
                le[i] = r[14 - i];
            }
            for (int i = 0; i < 8; i++) {
                s->resp[i] = le[2 * i] | (le[2 * i + 1] << 8);
            }
        }
    }
    s->stat0 |= STAT0_CMDRESPEND;

    if (data) {
        uint32_t n32 = s->regs[TMIO_BLKCOUNT32 / 2];
        uint32_t n16 = s->regs[TMIO_BLKCOUNT / 2];
        s->blocks_left = multi ? (n16 ? n16 : n32) : 1;
        s->blklen = s->regs[TMIO_BLKLEN32 / 2] ? s->regs[TMIO_BLKLEN32 / 2]
                                               : s->regs[TMIO_BLKLEN / 2];
        if (s->blklen == 0 || s->blklen > 512) {
            s->blklen = 512;
        }
        s->auto_stop = multi && (s->regs[TMIO_STOP / 2] & 0x100);
        s->pos = 0;
        if (read) {
            s->xfer_read = true;
            tmio_load_read_block(s);
        } else {
            s->xfer_write = true;
            s->regs[TMIO_DATACTL32 / 2] &= ~CTL32_TX32RQ;  /* ready for data */
            s->stat1 |= STAT1_TXRQ;
        }
    }
}

static uint32_t tmio_fifo_read(CtrTmioState *s, unsigned size)
{
    uint32_t v = 0;
    if (!s->xfer_read || !(s->regs[TMIO_DATACTL32 / 2] & CTL32_RX32RDY)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: FIFO read with no data\n");
        return 0;
    }
    for (unsigned i = 0; i < size && s->pos < s->blklen; i++) {
        v |= (uint32_t)s->block[s->pos++] << (8 * i);
    }
    if (s->pos >= s->blklen) {
        s->blocks_left--;
        s->regs[TMIO_DATACTL32 / 2] &= ~CTL32_RX32RDY;
        tmio_load_read_block(s);
    }
    return v;
}

static void tmio_fifo_write(CtrTmioState *s, uint32_t v, unsigned size)
{
    if (!s->xfer_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: FIFO write with no transfer\n");
        return;
    }
    for (unsigned i = 0; i < size && s->pos < s->blklen; i++) {
        s->block[s->pos++] = v >> (8 * i);
    }
    if (s->pos >= s->blklen) {
        sdbus_write_data(&s->sdbus, s->block, s->blklen);
        s->pos = 0;
        if (--s->blocks_left == 0) {
            tmio_finish_data(s);
        }
    }
}

static uint64_t tmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrTmioState *s = opaque;

    if (addr == TMIO_FIFO32 || addr == TMIO_FIFO16) {
        return tmio_fifo_read(s, size);
    }
    if (addr == TMIO_STAT0 && size == 4) {
        return tmio_live_stat0(s) | ((uint32_t)s->stat1 << 16);
    }
    switch (addr) {
    case TMIO_STAT0:
        return tmio_live_stat0(s);
    case TMIO_STAT1:
        return s->stat1;
    case TMIO_RESP0 ... TMIO_RESP0 + 15:
        if (size == 4) {
            return s->resp[(addr - TMIO_RESP0) / 2] |
                   ((uint32_t)s->resp[(addr - TMIO_RESP0) / 2 + 1] << 16);
        }
        return s->resp[(addr - TMIO_RESP0) / 2];
    default:
        if (addr < sizeof(s->regs)) {
            uint32_t v = s->regs[addr / 2];
            if (size == 4 && addr + 2 < sizeof(s->regs)) {
                v |= (uint32_t)s->regs[addr / 2 + 1] << 16;
            }
            return v;
        }
    }
    return 0;
}

static void tmio_write16(CtrTmioState *s, hwaddr addr, uint16_t v)
{
    switch (addr) {
    case TMIO_CMD:
        s->regs[0] = v;
        tmio_do_command(s, v);
        return;
    case TMIO_STAT0:
        s->stat0 &= v;           /* write-0-to-clear */
        return;
    case TMIO_STAT1:
        s->stat1 &= v;
        return;
    case TMIO_RESET:
        if (!(v & 1)) {
            s->stat0 = s->stat1 = 0;
            s->xfer_read = s->xfer_write = false;
        }
        break;
    }
    if (addr < sizeof(s->regs)) {
        s->regs[addr / 2] = v;
    }
}

static void tmio_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrTmioState *s = opaque;

    if (addr == TMIO_FIFO32 || addr == TMIO_FIFO16) {
        tmio_fifo_write(s, v, size);
        return;
    }
    if (size == 4) {
        tmio_write16(s, addr, v & 0xFFFF);
        tmio_write16(s, addr + 2, v >> 16);
    } else if (size == 2) {
        tmio_write16(s, addr, v);
    } else {
        uint16_t cur = tmio_read(s, addr & ~1, 2);
        cur = (addr & 1) ? ((cur & 0x00FF) | (v << 8)) : ((cur & 0xFF00) | (v & 0xFF));
        tmio_write16(s, addr & ~1, cur);
    }
}

static const MemoryRegionOps tmio_ops = {
    .read = tmio_read,
    .write = tmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void tmio_reset(DeviceState *dev)
{
    CtrTmioState *s = CTR_TMIO(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->stat0 = s->stat1 = 0;
    s->xfer_read = s->xfer_write = false;
}

static void tmio_init(Object *obj)
{
    CtrTmioState *s = CTR_TMIO(obj);
    memory_region_init_io(&s->iomem, obj, &tmio_ops, s, "ctr-tmio", 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(obj), "sd-bus");
}

static void tmio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, tmio_reset);
}

static const TypeInfo tmio_info = {
    .name = TYPE_CTR_TMIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CtrTmioState),
    .instance_init = tmio_init,
    .class_init = tmio_class_init,
};

static void tmio_register_types(void)
{
    type_register_static(&tmio_info);
}
type_init(tmio_register_types)

/* ------------------------------------------------------------------------ */
/* Machine state                                                            */

typedef struct {
    uint32_t regs[0x100 / 4];   /* PDC register file (offset/4) */
} CtrPdc;

#define PDC_FB_A0   (0x68 / 4)
#define PDC_FB_A1   (0x6C / 4)
#define PDC_FORMAT  (0x70 / 4)
#define PDC_SELECT  (0x78 / 4)
#define PDC_STRIDE  (0x90 / 4)

typedef struct {
    CPUClusterState cluster11, cluster9;   /* heterogeneous CPUs need separate clusters */
    ARMCPU *cpu9, *cpu11;
    MemoryRegion arm9_root;
    AddressSpace arm9_as;

    MemoryRegion fcram, vram, axiwram, dspram;
    MemoryRegion fcram9, vram9, axiwram9, dspram9;   /* aliases for ARM9 */
    MemoryRegion itcm, itcm_m1, itcm_m2, arm9ram, dtcm, boot9, boot11, boot11_m1, boot11_m2;
    MemoryRegion io_shared, io_shared9;              /* shared IO container */
    MemoryRegion cfg9, hid, cfg11, pdc;

    CtrPdc pdc_regs[2];
    qemu_irq pdc_irq[2];           /* GIC 42/43: VBlank */
    QEMUTimer *vblank;
    uint16_t hid_pad;            /* active low */
    uint16_t sdmmcctl;
    QemuConsole *con;

    uint32_t arm9_entry, arm11_entry;
    uint32_t argv_addr;
    bool screen_init;
} CtrMachine;

static CtrMachine *ctr;
extern uint32_t arm_mptimer_periphclk_hz;   /* emu/qemu/patches/0001 */

/* --- CFG9 (only SDMMCCTL is stored, rest reads 0) ------------------------ */
static uint64_t cfg9_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x20) {
        return s->sdmmcctl;
    }
    if (addr == 0x10000) {   /* CFG9_SYSPROT9 etc: report "locked" bootroms */
        return 1;
    }
    return 0;
}

static void cfg9_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x20) {
        s->sdmmcctl = v;
    }
}

static const MemoryRegionOps cfg9_ops = {
    .read = cfg9_read, .write = cfg9_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- HID ------------------------------------------------------------------ */
static uint64_t hid_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0) {
        return s->hid_pad;
    }
    return 0;
}

static void hid_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps hid_ops = {
    .read = hid_read, .write = hid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* 3DS HID bits: A B SELECT START RIGHT LEFT UP DOWN R L X Y */
static int qcode_to_hid(int q)
{
    switch (q) {
    case Q_KEY_CODE_A:      return 0;   /* A */
    case Q_KEY_CODE_S:      return 1;   /* B */
    case Q_KEY_CODE_BACKSPACE: return 2; /* SELECT */
    case Q_KEY_CODE_RET:    return 3;   /* START */
    case Q_KEY_CODE_RIGHT:  return 4;
    case Q_KEY_CODE_LEFT:   return 5;
    case Q_KEY_CODE_UP:     return 6;
    case Q_KEY_CODE_DOWN:   return 7;
    case Q_KEY_CODE_W:      return 8;   /* R */
    case Q_KEY_CODE_Q:      return 9;   /* L */
    case Q_KEY_CODE_X:      return 10;  /* X */
    case Q_KEY_CODE_Y:      return 11;  /* Y */
    default:                return -1;
    }
}

static void ctr_input_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    InputKeyEvent *key = evt->u.key.data;
    int bit = qcode_to_hid(qemu_input_key_value_to_qcode(key->key));
    if (bit < 0) {
        return;
    }
    if (key->down) {
        ctr->hid_pad &= ~(1u << bit);
    } else {
        ctr->hid_pad |= (1u << bit);
    }
}

static const QemuInputHandler ctr_input_handler = {
    .name = "3DS buttons",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = ctr_input_event,
};

/* --- CFG11 ---------------------------------------------------------------- */
static uint64_t cfg11_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 0xFFC) {     /* CFG11_SOCINFO: bit0 = CTR, bit1 = New3DS */
        return 1;
    }
    return 0;
}

static void cfg11_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps cfg11_ops = {
    .read = cfg11_read, .write = cfg11_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- PDC (two display controllers, 0x100 bytes each) --------------------- */
static uint64_t pdc_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    unsigned n = addr >> 8;
    return n < 2 ? s->pdc_regs[n].regs[(addr & 0xFF) / 4] : 0;
}

#define PDC_SWAP_ACK_V  (1u << 17)
#define PDC_CNT_NO_IRQ_V (1u << 9)

static void pdc_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    unsigned n = addr >> 8;
    if (n >= 2) {
        return;
    }
    if ((addr & 0xFF) == 0x78) {
        if (v & PDC_SWAP_ACK_V) {
            qemu_irq_lower(s->pdc_irq[n]);     /* acknowledge VBlank */
        }
        s->pdc_regs[n].regs[0x78 / 4] = v & 0x1;   /* ack bits are write-only */
        return;
    }
    s->pdc_regs[n].regs[(addr & 0xFF) / 4] = v;
}

/* 60 Hz VBlank: raise GIC 42/43 when the PDC is enabled with V IRQs on */
static void ctr_vblank(void *opaque)
{
    CtrMachine *s = opaque;
    for (int n = 0; n < 2; n++) {
        uint32_t cnt = s->pdc_regs[n].regs[0x74 / 4];
        if ((cnt & 1) && !(cnt & PDC_CNT_NO_IRQ_V)) {
            qemu_irq_raise(s->pdc_irq[n]);
        }
    }
    timer_mod(s->vblank, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
}

static const MemoryRegionOps pdc_ops = {
    .read = pdc_read, .write = pdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- Display: top screen at y=0, bottom screen centred at y=240 --------- */
static void ctr_draw_screen(CtrMachine *s, int n, uint32_t *dst, int dst_stride_px,
                            int width, int xoff, int yoff)
{
    CtrPdc *p = &s->pdc_regs[n];
    uint32_t fb = (p->regs[PDC_SELECT] & 1) ? p->regs[PDC_FB_A1] : p->regs[PDC_FB_A0];
    unsigned fmt = p->regs[PDC_FORMAT] & 7;
    unsigned bpp = (fmt == 0) ? 4 : (fmt == 1) ? 3 : 2;
    uint32_t stride = p->regs[PDC_STRIDE] ? p->regs[PDC_STRIDE] : CTR_SCR_H * bpp;
    g_autofree uint8_t *buf = NULL;
    size_t len = (size_t)stride * width;

    if (!fb || !(p->regs[0x74 / 4] & 1)) {
        return;      /* display disabled: leave black */
    }
    buf = g_malloc(len);
    if (address_space_read(&address_space_memory, fb, MEMTXATTRS_UNSPECIFIED, buf, len)
        != MEMTX_OK) {
        return;
    }
    for (int x = 0; x < width; x++) {
        const uint8_t *col = buf + (size_t)x * stride;
        for (int y = 0; y < CTR_SCR_H; y++) {
            /* memory column x holds screen column x, bottom-to-top */
            const uint8_t *px = col + (size_t)(CTR_SCR_H - 1 - y) * bpp;
            uint32_t r, g, b;
            switch (fmt) {
            case 0: r = px[3]; g = px[2]; b = px[1]; break;            /* ABGR8 {A,B,G,R} */
            case 1: r = px[2]; g = px[1]; b = px[0]; break;            /* BGR8 {B,G,R} */
            case 2: { uint16_t v = px[0] | px[1] << 8;                 /* RGB565 */
                      r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; break; }
            case 3: { uint16_t v = px[0] | px[1] << 8;                 /* RGB5A1 */
                      r = (v >> 11) << 3; g = ((v >> 6) & 31) << 3; b = ((v >> 1) & 31) << 3; break; }
            default: { uint16_t v = px[0] | px[1] << 8;                /* RGBA4 */
                      r = (v >> 12) << 4; g = ((v >> 8) & 15) << 4; b = ((v >> 4) & 15) << 4; break; }
            }
            dst[(size_t)(y + yoff) * dst_stride_px + x + xoff] = (r << 16) | (g << 8) | b;
        }
    }
}

static void ctr_gfx_update(void *opaque)
{
    CtrMachine *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t *d = surface_data(surf);
    int stride_px = surface_stride(surf) / 4;

    memset(d, 0x20, (size_t)surface_stride(surf) * surface_height(surf));
    ctr_draw_screen(s, 0, d, stride_px, CTR_TOP_W, 0, 0);
    ctr_draw_screen(s, 1, d, stride_px, CTR_BOT_W, (CTR_TOP_W - CTR_BOT_W) / 2, CTR_SCR_H);
    dpy_gfx_update_full(s->con);
}

static void ctr_gfx_invalidate(void *opaque)
{
}

static const GraphicHwOps ctr_gfx_ops = {
    .invalidate = ctr_gfx_invalidate,
    .gfx_update = ctr_gfx_update,
};

/* ------------------------------------------------------------------------ */
/* FIRM loading (mirrors Luma3DS loadHomebrewFirm/launchFirm)               */

static void ctr_load_firm(CtrMachine *s, const char *path)
{
    g_autofree uint8_t *data = NULL;
    gsize len;
    GError *err = NULL;

    if (!g_file_get_contents(path, (gchar **)&data, &len, &err)) {
        error_report("nintendo3ds: cannot read %s: %s", path, err->message);
        exit(1);
    }
    if (len < 0x200 || memcmp(data, "FIRM", 4) != 0) {
        error_report("nintendo3ds: %s is not a FIRM image", path);
        exit(1);
    }
    s->arm11_entry = ldl_le_p(data + 8);
    s->arm9_entry = ldl_le_p(data + 12);
    s->screen_init = data[0x10] & 1;
    for (int i = 0; i < 4; i++) {
        const uint8_t *h = data + 0x40 + 0x30 * i;
        uint32_t off = ldl_le_p(h), addr = ldl_le_p(h + 4), size = ldl_le_p(h + 8);
        if (!size) {
            continue;
        }
        if ((uint64_t)off + size > len) {
            error_report("nintendo3ds: FIRM section %d out of file bounds", i);
            exit(1);
        }
        g_autofree char *name = g_strdup_printf("firm.section%d", i);
        rom_add_blob_fixed_as(name, data + off, size, addr, &s->arm9_as);
        info_report("nintendo3ds: FIRM section %d -> 0x%08x (+0x%x)", i, addr, size);
    }

    /*
     * argv as Luma passes it: argv[0] = path string, argv[1] = struct fb[2]
     * (only when the FIRM requests screen init). Placed in DTCM like Luma
     * (which keeps it on its ITCM stack).
     */
    uint8_t blob[0x100] = { 0 };
    const char *p = "sdmc:/luma/payloads/ios3ds.firm";
    uint32_t base = CTR_DTCM_BASE + CTR_DTCM_SIZE - sizeof(blob);
    uint32_t fbs[6] = { 0x18300000, 0x18300000, 0x18346500,
                        0x18400000, 0x18400000, 0x18446500 };
    stl_le_p(blob + 0x00, base + 0x10);           /* argv[0] */
    stl_le_p(blob + 0x04, base + 0x80);           /* argv[1] */
    memcpy(blob + 0x10, p, strlen(p) + 1);
    for (int i = 0; i < 6; i++) {
        stl_le_p(blob + 0x80 + 4 * i, fbs[i]);
    }
    rom_add_blob_fixed_as("luma.argv", blob, sizeof(blob), base, &s->arm9_as);
    s->argv_addr = base;

    if (s->screen_init) {
        /* what Luma's screen init leaves behind (BGR8, stride 240*3, enabled) */
        for (int n = 0; n < 2; n++) {
            CtrPdc *pd = &s->pdc_regs[n];
            pd->regs[PDC_FB_A0] = n ? 0x18346500 : 0x18300000;
            pd->regs[PDC_FB_A1] = n ? 0x18446500 : 0x18400000;
            pd->regs[PDC_FORMAT] = n ? 0x80301 : 0x80341;
            pd->regs[PDC_STRIDE] = 0x2D0;
            pd->regs[PDC_SELECT] = 0;
            pd->regs[0x74 / 4] = 1;
        }
    }
}

static void ctr_reset_arm9(void *opaque)
{
    CtrMachine *s = opaque;
    CPUARMState *env = &s->cpu9->env;

    cpu_reset(CPU(s->cpu9));
    env->regs[0] = s->screen_init ? 2 : 1;
    env->regs[1] = s->argv_addr;
    env->regs[2] = 0x4BEEF;                /* boot9strap magic */
    env->cp15.sctlr_el[1] |= SCTLR_V;      /* Luma leaves high vectors on */
    arm_rebuild_hflags(env);
    cpu_set_pc(CPU(s->cpu9), s->arm9_entry);
}

static void ctr_reset_arm11(void *opaque)
{
    CtrMachine *s = opaque;

    cpu_reset(CPU(s->cpu11));
    if (s->arm11_entry) {
        cpu_set_pc(CPU(s->cpu11), s->arm11_entry);
    } else {
        /* ARM11 entry 0: core 0 spins in boot11 waiting on 0x1FFFFFFC */
        cpu_set_pc(CPU(s->cpu11), 0xFFFF0100);
    }
}

/*
 * Tiny ROMs that stand in for boot9/boot11: exception vectors that jump to
 * the RAM veneers the real ROMs jump to, plus (boot11) a loop that waits for
 * an entry point at 0x1FFFFFFC like Luma's ARM11 stub.
 */
static void ctr_make_vector_rom(uint32_t *rom, uint32_t ram_veneers, bool wait_loop)
{
    /* vector order: reset, undef, svc, pabt, dabt, reserved, irq, fiq.
     * RAM veneer order (Luma arm9 / boot11): irq, fiq, svc, undef, pabt, dabt */
    static const int veneer_idx[8] = { -1, 3, 2, 4, 5, -1, 0, 1 };
    for (int v = 0; v < 8; v++) {
        if (veneer_idx[v] < 0) {
            rom[v] = 0xEAFFFFFE;                       /* b . */
        } else {
            rom[v] = 0xE59FF018;                       /* ldr pc, [pc, #0x18] */
            rom[v + 8] = ram_veneers + 8 * veneer_idx[v];
        }
    }
    if (wait_loop) {
        /* at +0x100: wait for *(u32*)0x1FFFFFFC != 0, then bx to it */
        static const uint32_t loop[] = {
            0xE59F000C,  /* ldr r0, =0x1FFFFFFC */
            0xE5901000,  /* ldr r1, [r0]         */
            0xE3510000,  /* cmp r1, #0           */
            0x0AFFFFFC,  /* beq loop             */
            0xE12FFF11,  /* bx r1                */
            0x1FFFFFFC,
        };
        memcpy(&rom[0x100 / 4], loop, sizeof(loop));
    }
}

static void ctr_add_unimp(MemoryRegion *root, const char *name, hwaddr base, uint64_t size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion_overlap(root, base,
                                        sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), -1000);
}

static void ctr_init(MachineState *machine)
{
    CtrMachine *s = g_new0(CtrMachine, 1);
    MemoryRegion *sys = get_system_memory();   /* ARM11 view */
    Object *cpuobj;
    DeviceState *dev;

    ctr = s;
    s->hid_pad = 0xFFF;

    /* ---- address spaces ---- */
    memory_region_init(&s->arm9_root, NULL, "arm9-memory", UINT64_MAX);
    address_space_init(&s->arm9_as, &s->arm9_root, "arm9");

    /* ---- CPUs (one cluster each: different CPU types) ---- */
    object_initialize_child(OBJECT(machine), "arm11-cluster", &s->cluster11, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster11), "cluster-id", 0);
    cpuobj = object_new(ARM_CPU_TYPE_NAME("arm11mpcore"));
    object_property_add_child(OBJECT(&s->cluster11), "cpu0", cpuobj);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    s->cpu11 = ARM_CPU(cpuobj);
    qdev_realize(DEVICE(&s->cluster11), NULL, &error_fatal);

    object_initialize_child(OBJECT(machine), "arm9-cluster", &s->cluster9, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster9), "cluster-id", 1);
    cpuobj = object_new(ARM_CPU_TYPE_NAME("arm946"));
    object_property_add_child(OBJECT(&s->cluster9), "cpu0", cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(&s->arm9_root), &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    s->cpu9 = ARM_CPU(cpuobj);
    qdev_realize(DEVICE(&s->cluster9), NULL, &error_fatal);

    /* ---- shared RAM ---- */
    memory_region_init_ram(&s->fcram, NULL, "ctr.fcram", CTR_FCRAM_SIZE, &error_fatal);
    memory_region_init_ram(&s->vram, NULL, "ctr.vram", CTR_VRAM_SIZE, &error_fatal);
    memory_region_init_ram(&s->axiwram, NULL, "ctr.axiwram", CTR_AXIWRAM_SIZE, &error_fatal);
    memory_region_init_ram(&s->dspram, NULL, "ctr.dspram", CTR_DSPRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sys, CTR_FCRAM_BASE, &s->fcram);
    memory_region_add_subregion(sys, CTR_VRAM_BASE, &s->vram);
    memory_region_add_subregion(sys, CTR_AXIWRAM_BASE, &s->axiwram);
    memory_region_add_subregion(sys, CTR_DSPRAM_BASE, &s->dspram);
    memory_region_init_alias(&s->fcram9, NULL, "fcram9", &s->fcram, 0, CTR_FCRAM_SIZE);
    memory_region_init_alias(&s->vram9, NULL, "vram9", &s->vram, 0, CTR_VRAM_SIZE);
    memory_region_init_alias(&s->axiwram9, NULL, "axiwram9", &s->axiwram, 0, CTR_AXIWRAM_SIZE);
    memory_region_init_alias(&s->dspram9, NULL, "dspram9", &s->dspram, 0, CTR_DSPRAM_SIZE);
    memory_region_add_subregion(&s->arm9_root, CTR_FCRAM_BASE, &s->fcram9);
    memory_region_add_subregion(&s->arm9_root, CTR_VRAM_BASE, &s->vram9);
    memory_region_add_subregion(&s->arm9_root, CTR_AXIWRAM_BASE, &s->axiwram9);
    memory_region_add_subregion(&s->arm9_root, CTR_DSPRAM_BASE, &s->dspram9);

    /* ---- ARM9 private memory ---- */
    memory_region_init_ram(&s->itcm, NULL, "ctr.itcm", CTR_ITCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->arm9_root, CTR_ITCM_BASE, &s->itcm);
    memory_region_init_alias(&s->itcm_m1, NULL, "itcm.m1", &s->itcm, 0, CTR_ITCM_SIZE);
    memory_region_init_alias(&s->itcm_m2, NULL, "itcm.m2", &s->itcm, 0, CTR_ITCM_SIZE);
    memory_region_add_subregion(&s->arm9_root, 0x01FF8000, &s->itcm_m1);
    memory_region_add_subregion(&s->arm9_root, 0x07FF8000, &s->itcm_m2);
    memory_region_init_ram(&s->arm9ram, NULL, "ctr.arm9ram", CTR_ARM9RAM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->arm9_root, CTR_ARM9RAM_BASE, &s->arm9ram);
    memory_region_init_ram(&s->dtcm, NULL, "ctr.dtcm", CTR_DTCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->arm9_root, CTR_DTCM_BASE, &s->dtcm);

    /* ---- boot ROM stand-ins ---- */
    {
        uint32_t *rom = g_new0(uint32_t, CTR_BOOTROM_SIZE / 4);
        memory_region_init_rom(&s->boot9, NULL, "ctr.boot9", CTR_BOOTROM_SIZE, &error_fatal);
        memory_region_add_subregion(&s->arm9_root, CTR_BOOTROM_HI, &s->boot9);
        ctr_make_vector_rom(rom, CTR_A9_RAMVEC, false);
        rom_add_blob_fixed_as("boot9.vectors", rom, 0x400, CTR_BOOTROM_HI, &s->arm9_as);

        memset(rom, 0, CTR_BOOTROM_SIZE);
        memory_region_init_rom(&s->boot11, NULL, "ctr.boot11", CTR_BOOTROM_SIZE, &error_fatal);
        memory_region_add_subregion(sys, 0x00000000, &s->boot11);
        memory_region_init_alias(&s->boot11_m1, NULL, "boot11.m1", &s->boot11, 0, CTR_BOOTROM_SIZE);
        memory_region_init_alias(&s->boot11_m2, NULL, "boot11.m2", &s->boot11, 0, CTR_BOOTROM_SIZE);
        memory_region_add_subregion(sys, 0x00010000, &s->boot11_m1);
        memory_region_add_subregion(sys, CTR_BOOTROM_HI, &s->boot11_m2);
        ctr_make_vector_rom(rom, CTR_A11_RAMVEC, true);
        rom_add_blob_fixed("boot11.vectors", rom, 0x400, 0x00000000);
        g_free(rom);
    }

    /* ---- MPCore private region (SCU/GIC/timers) ---- */
    arm_mptimer_periphclk_hz = 268111856 / 2;   /* O3DS PERIPHCLK = CPU/2 */
    dev = qdev_new(TYPE_ARM11MPCORE_PRIV);
    qdev_prop_set_uint32(dev, "num-cpu", 1);
    qdev_prop_set_uint32(dev, "num-irq", 128);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sys, CTR_MPCORE_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu11), ARM_CPU_IRQ));
    s->pdc_irq[0] = qdev_get_gpio_in(dev, 42 - 32);
    s->pdc_irq[1] = qdev_get_gpio_in(dev, 43 - 32);
    s->vblank = timer_new_ns(QEMU_CLOCK_VIRTUAL, ctr_vblank, s);
    timer_mod(s->vblank, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);

    /* ---- shared IO 0x10100000-0x101FFFFF (visible to both CPUs) ---- */
    memory_region_init(&s->io_shared, NULL, "ctr.io-shared", 0x100000);
    memory_region_init_io(&s->hid, NULL, &hid_ops, s, "ctr.hid", 0x8);
    memory_region_add_subregion(&s->io_shared, CTR_HID_BASE - CTR_IO_SHARED_BASE, &s->hid);
    memory_region_init_io(&s->cfg11, NULL, &cfg11_ops, s, "ctr.cfg11", 0x1000);
    memory_region_add_subregion(&s->io_shared, CTR_CFG11_BASE - CTR_IO_SHARED_BASE, &s->cfg11);
    memory_region_add_subregion(sys, CTR_IO_SHARED_BASE, &s->io_shared);
    memory_region_init_alias(&s->io_shared9, NULL, "ctr.io-shared9", &s->io_shared, 0, 0x100000);
    memory_region_add_subregion(&s->arm9_root, CTR_IO_SHARED_BASE, &s->io_shared9);
    ctr_add_unimp(&s->io_shared, "ctr.io-shared.unimp", 0, 0x100000);

    /* ---- ARM9-only IO ---- */
    memory_region_init_io(&s->cfg9, NULL, &cfg9_ops, s, "ctr.cfg9", 0x1000);
    memory_region_add_subregion(&s->arm9_root, CTR_CFG9_BASE, &s->cfg9);
    dev = qdev_new(TYPE_CTR_TMIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(&s->arm9_root, CTR_TMIO1_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    {
        DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
        if (dinfo) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);
            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo), &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"), &error_fatal);
        } else {
            warn_report("nintendo3ds: no SD card (-drive if=sd,...); SD slot empty");
        }
    }
    ctr_add_unimp(&s->arm9_root, "ctr.io9.unimp", CTR_IO9_BASE, 0x100000);

    /* ---- ARM11-only IO ---- */
    memory_region_init_io(&s->pdc, NULL, &pdc_ops, s, "ctr.pdc", 0x200);
    memory_region_add_subregion(sys, CTR_PDC0_BASE, &s->pdc);
    ctr_add_unimp(sys, "ctr.io11.unimp", CTR_IO11_BASE, 0x400000);

    /* ---- display + input ---- */
    s->con = graphic_console_init(NULL, 0, &ctr_gfx_ops, s);
    qemu_console_resize(s->con, CTR_TOP_W, CTR_SCR_H * 2);
    qemu_input_handler_register(NULL, &ctr_input_handler);

    /* ---- payload ---- */
    if (!machine->kernel_filename) {
        error_report("nintendo3ds: use -kernel <payload.firm>");
        exit(1);
    }
    ctr_load_firm(s, machine->kernel_filename);
    qemu_register_reset(ctr_reset_arm11, s);
    qemu_register_reset(ctr_reset_arm9, s);
}

static void ctr_machine_init(MachineClass *mc)
{
    mc->desc = "Nintendo 3DS (Old 3DS subset for ios3ds development; Luma FIRM boot)";
    mc->init = ctr_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    /* unmapped physical accesses abort, like a real bus would (fail loudly) */
    mc->ignore_memory_transaction_failures = false;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm11mpcore");
    mc->auto_create_sdcard = true;
}

DEFINE_MACHINE_ARM("nintendo3ds", ctr_machine_init)
