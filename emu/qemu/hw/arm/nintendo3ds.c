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
 *   - the PICA200 GX block @0x10400000 (PSC memory fill, PPF transfer engine,
 *     P3D command-list processor) backed by the software model in
 *     tools/ios3ds-pica/sim/picasim.c, and PDN_GPU_CNT @0x10141200 reading
 *     "clocked, out of reset" as Luma leaves it (docs/pica200.md)
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
#include "system/runstate.h"
#include "system/rtc.h"
#include "qemu/bcd.h"
#include "hw/resettable.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "picasim.h"
#include "ctr_csnd.h"

/* ------------------------------------------------------------------------ */
/* Memory map                                                               */

#define CTR_ITCM_BASE        0x00000000
#define CTR_ITCM_SIZE        (32 * KiB)
#define CTR_ARM9RAM_BASE     0x08000000
#define CTR_ARM9RAM_SIZE     (1 * MiB)
#define CTR_IO9_BASE         0x10000000
#define CTR_CFG9_BASE        0x10000000
#define CTR_IRQ9_BASE        0x10001000
#define CTR_TIMER9_BASE      0x10003000
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

/* names and semantics: libn3ds include/drivers/tmio.h */
#define STAT0_CMDRESPEND  0x0001
#define STAT0_DATAEND     0x0004
#define STAT0_DETECT      0x0020  /* live: card detect */
#define STAT0_NO_WRPROT   0x0080  /* live: write-protect slider unlocked */
#define STAT0_DAT3_DETECT 0x0400  /* live: DAT3 high (card present and powered) */
#define STAT1_CMDTIMEOUT  0x0040
#define STAT1_SD_NOT_BUSY 0x0080  /* live: DAT0 high */
#define STAT1_RXRDY       0x0100
#define STAT1_TXRQ        0x0200
#define STAT1_CLKDIV_OK   0x2000  /* live: clock divider may be changed */
#define STAT1_CMD_BUSY    0x4000
#define TMIO_IRQ_BITS     0x837F031Du   /* maskable status bits, STAT0 | STAT1 << 16 */

#define CLKCTL_EN         0x0100
#define CTL32_EN          0x0002  /* 32 bit FIFO enable */
#define CTL32_FULL        0x0100  /* RO: a received block is waiting */
#define CTL32_NOT_EMPTY   0x0200  /* RO: 0 = empty, i.e. ready for TX data */
#define CTL32_CLEAR       0x0400  /* W: clear FIFO (pulse) */
#define CTL32_RW_BITS     0x1802

#define SDMMCCTL_CARD_PWR_OFF   0x0001
#define SDMMCCTL_CARD_TMIO1_SEL 0x0200

struct CtrTmioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;

    qemu_irq irq;          /* ARM9 IRQ 16: (status & ~mask) != 0 */

    uint16_t regs[0x200 / 2];
    uint16_t stat0, stat1; /* latched (acknowledgeable) status bits only */
    bool     rx_ready;     /* a read block is waiting in the FIFO */
    uint16_t resp[8];

    /* data phase */
    bool     xfer_read, xfer_write;
    uint32_t blocks_left;
    uint32_t blklen;
    uint8_t  block[512];
    uint32_t pos;          /* byte position inside block */
    bool     auto_stop;
};

static uint16_t ctr_sdmmcctl(void);

/* The SD slot is on this controller's port 0 only while CFG9_SDMMCCTL routes
 * it to TMIO1; port 1 (eMMC) is not modelled and never answers. */
static bool tmio_slot_selected(CtrTmioState *s)
{
    return (ctr_sdmmcctl() & SDMMCCTL_CARD_TMIO1_SEL) &&
           (s->regs[TMIO_PORTSEL / 2] & 3) == 0;
}

static bool tmio_card_reachable(CtrTmioState *s)
{
    return tmio_slot_selected(s) && sdbus_get_inserted(&s->sdbus) &&
           !(ctr_sdmmcctl() & SDMMCCTL_CARD_PWR_OFF) &&
           (s->regs[TMIO_CLKCTL / 2] & CLKCTL_EN);
}

/* idle with a writable card in the slot: STAT0 0x04A0, STAT1 0x2080 */
static uint16_t tmio_live_stat0(CtrTmioState *s)
{
    uint16_t v = s->stat0;
    if (tmio_slot_selected(s) && sdbus_get_inserted(&s->sdbus)) {
        v |= STAT0_DETECT;
        if (!(ctr_sdmmcctl() & SDMMCCTL_CARD_PWR_OFF)) {
            v |= STAT0_DAT3_DETECT;
        }
        if (!sdbus_get_readonly(&s->sdbus)) {
            v |= STAT0_NO_WRPROT;
        }
    }
    return v;
}

static uint16_t tmio_live_stat1(CtrTmioState *s)
{
    return s->stat1 | STAT1_SD_NOT_BUSY | STAT1_CLKDIV_OK;
}

static uint16_t tmio_datactl32(CtrTmioState *s)
{
    uint16_t v = s->regs[TMIO_DATACTL32 / 2] & CTL32_RW_BITS;
    if (s->rx_ready && (v & CTL32_EN)) {
        v |= CTL32_FULL;
    }
    return v;      /* NOT_EMPTY stays 0: written data is consumed at once */
}

static void tmio_update_irq(CtrTmioState *s)
{
    uint32_t st = s->stat0 | ((uint32_t)s->stat1 << 16);
    uint32_t mask = s->regs[TMIO_MASK0 / 2] | ((uint32_t)s->regs[TMIO_MASK1 / 2] << 16);
    qemu_set_irq(s->irq, (st & ~mask & TMIO_IRQ_BITS) != 0);
}

static void tmio_finish_data(CtrTmioState *s)
{
    s->xfer_read = s->xfer_write = false;
    s->rx_ready = false;
    s->stat0 |= STAT0_DATAEND;
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
    s->rx_ready = true;
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
    s->regs[TMIO_ERRSTAT0 / 2] = s->regs[TMIO_ERRSTAT1 / 2] = 0;
    s->xfer_read = s->xfer_write = false;
    s->rx_ready = false;
    if (tmio_card_reachable(s)) {
        rlen = sdbus_do_command(&s->sdbus, &rq, r, sizeof(r));
    } else {
        /* slot routed elsewhere / powered off / clock stopped / eMMC port */
        qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: CMD%d with no reachable card "
                      "(SDMMCCTL %04x PORTSEL %04x CLKCTL %04x)\n", rq.cmd, ctr_sdmmcctl(),
                      s->regs[TMIO_PORTSEL / 2], s->regs[TMIO_CLKCTL / 2]);
        rlen = 0;
        rtype = 4;          /* nothing drives CMD: every command times out */
    }

    memset(s->resp, 0, sizeof(s->resp));
    if (rtype >= 4 || (rtype == 0 && rq.cmd != 0)) {
        if (rlen == 0) {
            /* response start bit timeout; the controller still ends the
             * command, so RESP_END is set along with the error */
            s->stat1 |= STAT1_CMDTIMEOUT;
            s->stat0 |= STAT0_CMDRESPEND;
            s->regs[TMIO_ERRSTAT1 / 2] |= 1;    /* ERR_CMD_RESP_TMOUT */
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
            s->stat1 |= STAT1_TXRQ;
        }
    }
}

static uint32_t tmio_fifo_read(CtrTmioState *s, unsigned size)
{
    uint32_t v = 0;
    if (!s->xfer_read || !s->rx_ready) {
        qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: FIFO read with no data\n");
        return 0;
    }
    for (unsigned i = 0; i < size && s->pos < s->blklen; i++) {
        v |= (uint32_t)s->block[s->pos++] << (8 * i);
    }
    if (s->pos >= s->blklen) {
        s->blocks_left--;
        s->rx_ready = false;
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
        uint32_t v;
        if (addr == TMIO_FIFO32 && !(s->regs[TMIO_DATACTL32 / 2] & CTL32_EN)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: FIFO32 read while disabled\n");
            return 0;
        }
        v = tmio_fifo_read(s, size);
        tmio_update_irq(s);
        return v;
    }
    if (addr == TMIO_STAT0 && size == 4) {
        return tmio_live_stat0(s) | ((uint32_t)tmio_live_stat1(s) << 16);
    }
    switch (addr) {
    case TMIO_STAT0:
        return tmio_live_stat0(s);
    case TMIO_STAT1:
        return tmio_live_stat1(s);
    case TMIO_DATACTL32:
        return tmio_datactl32(s);
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
    case TMIO_PORTSEL:
        v &= 0x0403;             /* port + the one other writable bit */
        break;
    case TMIO_DATACTL32:
        v &= CTL32_RW_BITS;      /* FULL / NOT_EMPTY are read-only status */
        break;
    case TMIO_ERRSTAT0:
    case TMIO_ERRSTAT1:
        return;                  /* read-only */
    case TMIO_RESET:
        if (!(v & 1)) {
            s->stat0 = s->stat1 = 0;
            s->xfer_read = s->xfer_write = false;
            s->rx_ready = false;
            s->regs[TMIO_MASK0 / 2] = TMIO_IRQ_BITS & 0xFFFF;
            s->regs[TMIO_MASK1 / 2] = TMIO_IRQ_BITS >> 16;
            s->regs[TMIO_STOP / 2] = 0;
            memset(s->resp, 0, sizeof(s->resp));
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
        if (addr == TMIO_FIFO32 && !(s->regs[TMIO_DATACTL32 / 2] & CTL32_EN)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ctr-tmio: FIFO32 write while disabled\n");
            return;
        }
        tmio_fifo_write(s, v, size);
        tmio_update_irq(s);
        return;
    }
    if (size == 4) {
        tmio_write16(s, addr, v & 0xFFFF);
        tmio_write16(s, addr + 2, v >> 16);
    } else if (size == 2) {
        tmio_write16(s, addr, v);
    } else {
        uint16_t cur = tmio_read(s, addr & ~1, 2);
        if ((addr & ~1) == TMIO_STAT0 || (addr & ~1) == TMIO_STAT1) {
            cur = 0xFFFF;        /* write-0-to-clear: leave the other byte alone */
        }
        cur = (addr & 1) ? ((cur & 0x00FF) | (v << 8)) : ((cur & 0xFF00) | (v & 0xFF));
        tmio_write16(s, addr & ~1, cur);
    }
    tmio_update_irq(s);
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
    s->rx_ready = false;
    s->regs[TMIO_MASK0 / 2] = TMIO_IRQ_BITS & 0xFFFF;   /* everything masked */
    s->regs[TMIO_MASK1 / 2] = TMIO_IRQ_BITS >> 16;
    tmio_update_irq(s);
}

static void tmio_init(Object *obj)
{
    CtrTmioState *s = CTR_TMIO(obj);
    memory_region_init_io(&s->iomem, obj, &tmio_ops, s, "ctr-tmio", 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
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
    uint32_t irq_stat;          /* bit0 H, bit1 V, bit2 error (PDC_SWAP 16..18) */
    bool     cur_fb;            /* framebuffer being scanned out (PDC_SWAP bit 4) */
} CtrPdc;

#define PDC_V_COUNT (0x54 / 4)
#define PDC_FB_A0   (0x68 / 4)
#define PDC_FB_A1   (0x6C / 4)
#define PDC_FORMAT  (0x70 / 4)
#define PDC_CNT     (0x74 / 4)
#define PDC_SELECT  (0x78 / 4)
#define PDC_STRIDE  (0x90 / 4)

#define PDC_CNT_EN        (1u << 0)
#define PDC_CNT_NO_IRQ_H  (1u << 8)
#define PDC_IRQ_H         1u
#define PDC_IRQ_V         2u
#define PDC_LINES         413           /* v_total of the 240p presets */
#define PDC_LINE_NS       (NANOSECONDS_PER_SECOND / 60 / PDC_LINES)

/* New-interface SPI bus ("NSPI", libn3ds spi.c) */
typedef struct {
    MemoryRegion iomem;
    int      bus;               /* 0..2 = SPI bus 1..3 */
    qemu_irq irq;
    uint32_t cnt, blklen, autopoll, int_mask, int_stat;
    uint32_t left;              /* bytes left in the running transfer */
    bool     cs_low;            /* chip select asserted (until NSPI_CS is written) */
} CtrNspi;

/* CTR CODEC ("TSC"): page/register file + touch / circle pad ADC */
typedef struct {
    uint8_t page;
    uint8_t regs[256][128];
    bool    in_data;            /* command byte seen in this chip-select window */
    bool    read;
    uint8_t reg;
    uint8_t adc[0x34];
} CtrCodec;

/* I2C controller (libn3ds i2c.c) */
typedef struct {
    MemoryRegion iomem;
    int      bus;               /* 0..2 = I2C bus 1..3 */
    qemu_irq irq;
    uint8_t  data, cnt;
    uint8_t  cntex[2], scl[2];
    bool     active;            /* between START and STOP */
    bool     dev_ok;            /* addressed device exists */
    uint8_t  addr;              /* 8 bit device address incl. R/W bit */
} CtrI2c;

/* MCU (I2C bus 2, device 0x4A) */
typedef struct {
    uint8_t  regs[256];
    uint8_t  reg;               /* register pointer */
    bool     got_reg;
    uint32_t irq, irq_mask;     /* regs 0x10-0x13 / 0x18-0x1B */
    time_t   rtc_offset;        /* guest RTC - host clock, seconds */
    QEMUTimer *power_held;
    uint8_t  orient;            /* accelerometer pose ('[' / ']', qom mcu-orient), 0..3 */
} CtrMcu;

typedef struct {
    uint16_t reload, cnt;
    uint32_t counter;           /* value at 'stamp' (16 bit, kept wider for math) */
    int64_t  stamp;             /* QEMU_CLOCK_VIRTUAL ns of the last sync */
    QEMUTimer *t;
} CtrTimer9;

typedef struct {
    CPUClusterState cluster11, cluster9;   /* heterogeneous CPUs need separate clusters */
    ARMCPU *cpu9, *cpu11;
    MemoryRegion arm9_root;
    AddressSpace arm9_as;

    MemoryRegion fcram, vram, axiwram, dspram;
    MemoryRegion fcram9, vram9, axiwram9, dspram9;   /* aliases for ARM9 */
    MemoryRegion itcm, itcm_m1, itcm_m2, arm9ram, dtcm, boot9, boot11, boot11_m1, boot11_m2;
    MemoryRegion io_shared, io_shared9;              /* shared IO container */
    MemoryRegion cfg9, hid, cfg11, pdc, irq9, timer9, gx, pdn;

    DeviceState *gic;
    DeviceState *sdcard;

    CtrPdc pdc_regs[2];
    qemu_irq pdc_irq[2];           /* GIC 42/43: PDC0/PDC1 H, V and error */
    PicaSim pica;                  /* GX block: PSC, PPF, P3D */
    qemu_irq gx_irq[4];            /* GIC 40 PSC0, 41 PSC1, 44 PPF, 45 P3D */
    uint32_t gpu_cnt, gpuprot;     /* PDN_GPU_CNT, CFG11_GPUPROT */
    struct { uint32_t off, val; int64_t due; } gx_q[8];   /* started GPU operations, not run yet */
    int gx_nq;
    QEMUTimer *gx_timer;
    QEMUTimer *vblank, *hblank;
    int64_t vblank_stamp;
    uint16_t hid_pad;            /* active low */
    uint16_t sdmmcctl;
    uint16_t spi_cnt;            /* CFG11_SPI_CNT: bit n = bus n+1 on the NSPI interface */
    QemuConsole *con;

    /* ARM9 interrupt controller + timers */
    uint32_t irq9_ie, irq9_if, irq9_level;
    qemu_irq *irq9_in;
    CtrTimer9 tmr9[4];

    /* SPI / CODEC / I2C / MCU */
    CtrNspi nspi[3];
    CtrCodec codec;
    CtrI2c i2c[3];
    CtrMcu mcu;
    qemu_irq mcu_irq;              /* GIC 113 (GPIO3 bit 9, falling edge) */
    MemoryRegion lcd;              /* LCD registers 0x10202000 (backlight PWM) */
    uint32_t lcd_abl[2];           /* top / bottom backlight level */
    bool lcd_abl_set;              /* the guest wrote one: the display shows it */

    /* pointer + circle pad state from the QEMU console */
    int  ptr_x, ptr_y;             /* display pixels, 0..399 / 0..479 */
    bool ptr_down;
    int  cpad_x, cpad_y;           /* -1, 0, +1 */

    uint32_t arm9_entry, arm11_entry;
    uint32_t argv_addr;
    bool screen_init;
} CtrMachine;

static CtrMachine *ctr;
extern uint32_t arm_mptimer_periphclk_hz;   /* emu/qemu/patches/0001 */

static uint16_t ctr_sdmmcctl(void)
{
    return ctr ? ctr->sdmmcctl : 0;
}

/* Sub-word access helpers: registers are modelled as 32 bit words; byte and
 * halfword reads pick their lane, writes must start at the low byte. */
static inline uint64_t reg_lane(uint32_t v32, hwaddr addr)
{
    return v32 >> ((addr & 3) * 8);
}

/* --- CFG9 (SYSPROT, SDMMCCTL, SOCINFO; rest reads 0) --------------------- */
static uint64_t cfg9_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    switch (addr & ~3) {
    case 0x00:      /* SYSPROT9 = 3, SYSPROT11 = 1: both boot ROMs and the OTP locked */
        return reg_lane(0x0103, addr);
    case 0x20:
        return reg_lane(s->sdmmcctl, addr);
    case 0xFFC:     /* SOCINFO: CTR */
        return reg_lane(1, addr);
    }
    return 0;
}

static void cfg9_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    uint16_t old = s->sdmmcctl;

    if (addr == 0x20) {
        s->sdmmcctl = size == 1 ? ((old & 0xFF00) | (v & 0xFF)) : (v & 0xFFFF);
    } else if (addr == 0x21 && size == 1) {
        s->sdmmcctl = (old & 0x00FF) | ((v & 0xFF) << 8);
    } else {
        return;
    }
    if (!(old & SDMMCCTL_CARD_PWR_OFF) && (s->sdmmcctl & SDMMCCTL_CARD_PWR_OFF) && s->sdcard) {
        device_cold_reset(s->sdcard);      /* slot power removed: the card forgets its state */
    }
}

static const MemoryRegionOps cfg9_ops = {
    .read = cfg9_read, .write = cfg9_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- ARM9 interrupt controller 0x10001000 (libn3ds arm9 interrupt.c) ----- */
#define IRQ9_TIMER0   8
#define IRQ9_TMIO1    16
#define IRQ9_LINES    30

static void irq9_update(CtrMachine *s)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu9), ARM_CPU_IRQ), (s->irq9_ie & s->irq9_if) != 0);
}

/* sources are edge-latched into IRQ_IF */
static void irq9_set(void *opaque, int n, int level)
{
    CtrMachine *s = opaque;
    uint32_t bit = 1u << n;

    if (level && !(s->irq9_level & bit)) {
        s->irq9_if |= bit;
    }
    s->irq9_level = level ? (s->irq9_level | bit) : (s->irq9_level & ~bit);
    irq9_update(s);
}

static uint64_t irq9_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    switch (addr & ~3) {
    case 0x0: return reg_lane(s->irq9_ie, addr);
    case 0x4: return reg_lane(s->irq9_if, addr);
    }
    return 0;
}

static void irq9_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    uint32_t mask = (size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1)) << ((addr & 3) * 8);
    uint32_t val = (uint32_t)v << ((addr & 3) * 8);

    switch (addr & ~3) {
    case 0x0:
        s->irq9_ie = ((s->irq9_ie & ~mask) | (val & mask)) & ((1u << IRQ9_LINES) - 1);
        break;
    case 0x4:
        s->irq9_if &= ~(val & mask);       /* write 1 to acknowledge */
        break;
    }
    irq9_update(s);
}

static const MemoryRegionOps irq9_ops = {
    .read = irq9_read, .write = irq9_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- ARM9 timers 0x10003000: 4 x 16 bit up counters (libn3ds arm9 timer.h) */
#define TIMER9_HZ        67027964
#define TIMER9_COUNT_UP  (1u << 2)
#define TIMER9_IRQ_EN    (1u << 6)
#define TIMER9_EN        (1u << 7)

static bool tmr9_free_running(CtrTimer9 *t, int n)
{
    return (t->cnt & TIMER9_EN) && !(n > 0 && (t->cnt & TIMER9_COUNT_UP));
}

static uint64_t tmr9_period_ns(CtrTimer9 *t)   /* ns per tick, x1024 fixed point */
{
    static const unsigned presc[4] = { 1, 64, 256, 1024 };
    return (uint64_t)NANOSECONDS_PER_SECOND * 1024 * presc[t->cnt & 3] / TIMER9_HZ;
}

/* add 'ticks' to timer n; overflows reload, raise the IRQ and clock a cascaded
 * successor */
static void tmr9_advance(CtrMachine *s, int n, uint64_t ticks)
{
    while (n < 4 && ticks) {
        CtrTimer9 *t = &s->tmr9[n];
        uint64_t span = 0x10000 - t->counter;
        uint64_t overflows = 0;

        if (ticks < span) {
            t->counter += ticks;
        } else {
            uint64_t per = 0x10000 - t->reload;
            ticks -= span;
            overflows = 1 + ticks / per;
            t->counter = t->reload + ticks % per;
        }
        if (overflows && (t->cnt & TIMER9_IRQ_EN)) {
            qemu_irq_pulse(s->irq9_in[IRQ9_TIMER0 + n]);
        }
        n++;
        if (n < 4 && (s->tmr9[n].cnt & (TIMER9_EN | TIMER9_COUNT_UP)) ==
                     (TIMER9_EN | TIMER9_COUNT_UP)) {
            ticks = overflows;
        } else {
            ticks = 0;
        }
    }
}

static void tmr9_sync(CtrMachine *s, int n)
{
    CtrTimer9 *t = &s->tmr9[n];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (tmr9_free_running(t, n)) {
        uint64_t per = tmr9_period_ns(t);
        uint64_t ticks = (uint64_t)(now - t->stamp) * 1024 / per;
        t->stamp += ticks * per / 1024;
        tmr9_advance(s, n, ticks);
    } else {
        t->stamp = now;
    }
}

/* wake up at the next overflow when someone needs to see it (IRQ / cascade) */
static void tmr9_arm(CtrMachine *s, int n)
{
    CtrTimer9 *t = &s->tmr9[n];
    bool cascade = n < 3 && (s->tmr9[n + 1].cnt & (TIMER9_EN | TIMER9_COUNT_UP)) ==
                            (TIMER9_EN | TIMER9_COUNT_UP);

    if (tmr9_free_running(t, n) && ((t->cnt & TIMER9_IRQ_EN) || cascade)) {
        uint64_t ns = (uint64_t)(0x10000 - t->counter) * tmr9_period_ns(t) / 1024;
        /* very fast overflow rates are batched (tmr9_advance counts them) */
        timer_mod(t->t, t->stamp + MAX(ns, 50 * SCALE_US) + 1);
    } else {
        timer_del(t->t);
    }
}

static void tmr9_tick(void *opaque)
{
    CtrTimer9 *t = opaque;
    int n = t - ctr->tmr9;

    tmr9_sync(ctr, n);
    tmr9_arm(ctr, n);
}

static uint64_t tmr9_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    int n = (addr >> 2) & 3;

    for (int i = 0; i <= n; i++) {     /* cascades are clocked by lower timers */
        tmr9_sync(s, i);
    }
    return reg_lane((s->tmr9[n].counter & 0xFFFF) | ((uint32_t)s->tmr9[n].cnt << 16), addr);
}

static void tmr9_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    int n = (addr >> 2) & 3;
    CtrTimer9 *t = &s->tmr9[n];

    for (int i = 0; i < 4; i++) {
        tmr9_sync(s, i);
    }
    if ((addr & 3) == 0) {
        t->reload = v & 0xFFFF;         /* TIMER_VAL write sets the reload value */
        if (size == 4) {
            addr += 2;
            v >>= 16;
        }
    }
    if ((addr & 3) == 2) {
        uint16_t old = t->cnt;
        t->cnt = v & 0xC7;
        if (!(old & TIMER9_EN) && (t->cnt & TIMER9_EN)) {
            t->counter = t->reload;     /* enabling loads the counter */
            t->stamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
    }
    for (int i = 0; i < 4; i++) {
        tmr9_arm(s, i);
    }
}

static const MemoryRegionOps tmr9_ops = {
    .read = tmr9_read, .write = tmr9_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- HID ------------------------------------------------------------------ */
static uint64_t hid_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    if ((addr & ~3) == 0) {
        return reg_lane(s->hid_pad, addr);
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

/* --- CODEC on SPI bus 2, chip select 0 (libn3ds codec.c) ------------------ */
#define CDC_PAGE_TOUCH   0x67
#define CDC_PAGE_ADC     0xFB
#define CDC_TOUCH_CNT    0x24       /* bit 7: conversion stopped */
#define CDC_TOUCH_STAT   0x26       /* bit 7: touch enable, bit 1 (RO): no new data */
#define CPAD_RANGE       1200       /* raw ADC swing around 2048 at full tilt */

static bool codec_adc_running(CtrCodec *c)
{
    return !(c->regs[CDC_PAGE_TOUCH][CDC_TOUCH_CNT] & 0x80) &&
           (c->regs[CDC_PAGE_TOUCH][CDC_TOUCH_STAT] & 0x80);
}

/* Bottom screen occupies x 40..359, y 240..479 of the 400x480 display. The
 * raw ADC range 0..4095 spans the panel (libn3ds hid.c: x * 320 / 4096). */
static void codec_sample(CtrMachine *s)
{
    CtrCodec *c = &s->codec;
    int bx = s->ptr_x - (CTR_TOP_W - CTR_BOT_W) / 2, by = s->ptr_y - CTR_SCR_H;
    bool pen = s->ptr_down && bx >= 0 && bx < CTR_BOT_W && by >= 0 && by < CTR_SCR_H;
    uint16_t tx = pen ? (bx * 4096 + 2048) / CTR_BOT_W : 0x1FFF;     /* bit 12: pen up */
    uint16_t ty = pen ? (by * 4096 + 2048) / CTR_SCR_H : 0x1FFF;
    uint16_t cy = 2048 + s->cpad_y * CPAD_RANGE;                     /* up = larger */
    uint16_t cx = 2048 - s->cpad_x * CPAD_RANGE;                     /* X is inverted */

    for (int i = 0; i < 5; i++) {
        stw_be_p(c->adc + i * 2, tx);
        stw_be_p(c->adc + 10 + i * 2, ty);
    }
    for (int i = 0; i < 8; i++) {
        stw_be_p(c->adc + 20 + i * 2, cy);
        stw_be_p(c->adc + 36 + i * 2, cx);
    }
}

static void codec_select(CtrMachine *s, bool low)
{
    s->codec.in_data = false;
}

static uint8_t codec_xfer(CtrMachine *s, uint8_t out)
{
    CtrCodec *c = &s->codec;
    uint8_t v = 0;

    if (!c->in_data) {              /* command byte: reg << 1 | read */
        c->in_data = true;
        c->reg = out >> 1;
        c->read = out & 1;
        if (c->read && c->page == CDC_PAGE_ADC) {
            codec_sample(s);
        }
        return 0;
    }
    if (c->read) {
        if (c->reg == 0) {
            v = c->page;
        } else if (c->page == CDC_PAGE_ADC) {
            v = c->reg <= sizeof(c->adc) ? c->adc[c->reg - 1] : 0;
        } else {
            v = c->regs[c->page][c->reg];
            if (c->page == CDC_PAGE_TOUCH && c->reg == CDC_TOUCH_STAT) {
                v = (v & ~2) | (codec_adc_running(c) ? 0 : 2);
            }
        }
    } else if (c->reg == 0) {
        c->page = out;              /* register 0 of every page selects the page */
    } else {
        c->regs[c->page][c->reg] = out;
    }
    c->reg = (c->reg + 1) & 0x7F;
    return v;
}

static void codec_reset(CtrCodec *c)
{
    memset(c, 0, sizeof(*c));
    c->regs[CDC_PAGE_TOUCH][CDC_TOUCH_CNT] = 0x80;     /* ADC stopped until initialised */
}

/* --- NSPI buses ----------------------------------------------------------- */
#define NSPI_CNT_DIR_S   (1u << 13)
#define NSPI_CNT_EN      (1u << 15)
#define NSPI_INT_TRAN_END 1u
#define NSPI_INT_AP_MATCH 2u
#define NSPI_INT_AP_TMOUT 4u

static bool nspi_has_codec(CtrMachine *s, CtrNspi *n)
{
    /* bus 2, CS0, and CFG11_SPI_CNT must route the bus to this interface */
    return n->bus == 1 && ((n->cnt >> 6) & 3) == 0 && (s->spi_cnt & (1u << n->bus));
}

static uint8_t nspi_byte(CtrMachine *s, CtrNspi *n, uint8_t out)
{
    if (!nspi_has_codec(s, n)) {
        return 0xFF;                /* nobody drives MISO */
    }
    if (!n->cs_low) {
        n->cs_low = true;
        codec_select(s, true);
    }
    return codec_xfer(s, out);
}

static void nspi_deselect(CtrMachine *s, CtrNspi *n)
{
    if (n->cs_low) {
        n->cs_low = false;
        codec_select(s, false);
    }
}

static void nspi_update_irq(CtrNspi *n)
{
    qemu_set_irq(n->irq, (n->int_stat & ~n->int_mask & 7) != 0);
}

static void nspi_done(CtrNspi *n)
{
    n->cnt &= ~NSPI_CNT_EN;
    n->int_stat |= NSPI_INT_TRAN_END;
    nspi_update_irq(n);
}

static uint64_t nspi_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrNspi *n = opaque;
    CtrMachine *s = ctr;
    uint32_t v = 0;

    switch (addr & ~3) {
    case 0x00: v = n->cnt; break;
    case 0x08: v = n->blklen; break;
    case 0x0C:
        if ((addr & 3) || !(n->cnt & NSPI_CNT_EN) || (n->cnt & NSPI_CNT_DIR_S)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ctr-nspi%d: FIFO read with no receive running\n",
                          n->bus + 1);
            return 0;
        }
        for (int i = 0; i < 4 && n->left; i++, n->left--) {
            v |= (uint32_t)nspi_byte(s, n, 0) << (8 * i);
        }
        if (!n->left) {
            nspi_done(n);
        }
        return v;
    case 0x10: v = 0; break;        /* FIFO_STAT: never busy, transfers are instant */
    case 0x14: v = n->autopoll; break;
    case 0x18: v = n->int_mask; break;
    case 0x1C: v = n->int_stat; break;
    }
    return reg_lane(v, addr);
}

static void nspi_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrNspi *n = opaque;
    CtrMachine *s = ctr;

    if (addr & 3) {
        qemu_log_mask(LOG_GUEST_ERROR, "ctr-nspi%d: unaligned write @0x%x\n",
                      n->bus + 1, (unsigned)addr);
        return;
    }
    switch (addr) {
    case 0x00:
        n->cnt = v & 0xB0C7;
        if (n->cnt & NSPI_CNT_EN) {
            n->left = n->blklen;
            if (!n->left) {
                nspi_done(n);
            }
        }
        break;
    case 0x04:                      /* NSPI_CS: writing releases chip select */
        nspi_deselect(s, n);
        break;
    case 0x08:
        n->blklen = v & 0x1FFFFF;
        break;
    case 0x0C:
        if (!(n->cnt & NSPI_CNT_EN) || !(n->cnt & NSPI_CNT_DIR_S)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ctr-nspi%d: FIFO write with no send running\n",
                          n->bus + 1);
            return;
        }
        for (int i = 0; i < 4 && n->left; i++, n->left--) {
            nspi_byte(s, n, v >> (8 * i));
        }
        if (!n->left) {
            nspi_done(n);
        }
        break;
    case 0x14:
        n->autopoll = v & 0x7FFFFFFF;
        if (v & (1u << 31)) {       /* send the command, test one response bit */
            uint8_t r;
            nspi_byte(s, n, v & 0xFF);
            r = nspi_byte(s, n, 0);
            nspi_deselect(s, n);
            /* a real bus retries until the timeout; our devices never change
             * state on their own, so one try decides */
            n->int_stat |= NSPI_INT_TRAN_END |
                (((r >> ((v >> 24) & 7)) & 1) == ((v >> 30) & 1) ? NSPI_INT_AP_MATCH
                                                                 : NSPI_INT_AP_TMOUT);
            nspi_update_irq(n);
        }
        break;
    case 0x18:
        n->int_mask = v & 7;
        nspi_update_irq(n);
        break;
    case 0x1C:
        n->int_stat &= ~(v & 7);    /* write 1 to acknowledge */
        nspi_update_irq(n);
        break;
    }
}

static const MemoryRegionOps nspi_ops = {
    .read = nspi_read, .write = nspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- MCU (libn3ds mcu_regmap.h, linux-3ds mcu/rtc.c) ---------------------- */
#define MCU_REG_IRQ        0x10
#define MCU_REG_IRQ_MASK   0x18
#define MCU_REG_SYS_PWR    0x20
#define MCU_REG_LCD_PWR    0x22
#define MCU_REG_RTC_SEC    0x30
#define MCU_REG_RTC_YEAR   0x36
#define MCU_REG_EX_HW_STAT 0x0F
#define MCU_IRQ_POWER_PRESS  (1u << 0)
#define MCU_IRQ_POWER_HELD   (1u << 1)
#define MCU_IRQ_HOME_PRESS   (1u << 2)
#define MCU_IRQ_HOME_RELEASE (1u << 3)
#define MCU_IRQ_SHELL_CLOSE  (1u << 5)
#define MCU_IRQ_SHELL_OPEN   (1u << 6)
#define MCU_IRQ_VOL_CHANGE   (1u << 22)
#define MCU_REG_ACC_CFG    0x40
#define MCU_REG_ACC_DATA   0x45     /* x, y, z: s16 little endian, 0x45..0x4A */
#define MCU_ACC_1G         0x4000   /* model choice: 1 g = 16384 (docs/mcu.md) */

/*
 * Accelerometer poses for the '[' / ']' keys and the mcu-orient property.
 * Model axes (docs/mcu.md, unverified on hardware): x = towards the right
 * side of the console, y = towards the hinge, z = out of the lower screen;
 * the sensor reports +1 g against gravity. 0 = iOS upright (console turned
 * 90 degrees clockwise: the left side is up), then 90 degree steps.
 */
static const int16_t mcu_poses[4][3] = {
    { -MCU_ACC_1G, 0, 0 },          /* left side up: iOS portrait */
    { 0, -MCU_ACC_1G, 0 },          /* hinge down: iOS landscape (home button left) */
    { MCU_ACC_1G, 0, 0 },           /* right side up: iOS upside down */
    { 0, MCU_ACC_1G, 0 },           /* hinge up (held normally, upright): iOS landscape */
};

static void mcu_set_pose(CtrMcu *m, int pose)
{
    m->orient = pose & 3;
}

static void mcu_raise(CtrMachine *s, uint32_t bits)
{
    CtrMcu *m = &s->mcu;

    bits &= ~m->irq_mask;           /* masked events are not latched */
    if (bits & ~m->irq) {
        m->irq |= bits;
        qemu_irq_pulse(s->mcu_irq); /* MCU IRQ pin: falling edge on GPIO3 bit 9 */
    }
}

static void mcu_power_held(void *opaque)
{
    mcu_raise(opaque, MCU_IRQ_POWER_HELD);
}

static uint8_t mcu_read_reg(CtrMachine *s, uint8_t reg)
{
    CtrMcu *m = &s->mcu;

    if (reg >= MCU_REG_IRQ && reg < MCU_REG_IRQ + 4) {       /* clear on read */
        unsigned sh = (reg - MCU_REG_IRQ) * 8;
        uint8_t v = m->irq >> sh;
        m->irq &= ~(0xFFu << sh);
        return v;
    }
    if (reg >= MCU_REG_IRQ_MASK && reg < MCU_REG_IRQ_MASK + 4) {
        return m->irq_mask >> ((reg - MCU_REG_IRQ_MASK) * 8);
    }
    if (reg >= MCU_REG_RTC_SEC && reg <= MCU_REG_RTC_YEAR) {  /* BCD */
        struct tm tm;
        int v;
        qemu_get_timedate(&tm, m->rtc_offset);
        switch (reg - MCU_REG_RTC_SEC) {
        case 0: v = tm.tm_sec; break;
        case 1: v = tm.tm_min; break;
        case 2: v = tm.tm_hour; break;
        case 3: v = tm.tm_wday; break;
        case 4: v = tm.tm_mday; break;
        case 5: v = tm.tm_mon + 1; break;
        default: v = tm.tm_year % 100; break;                 /* years since 2000 */
        }
        return to_bcd(v);
    }
    if (reg >= MCU_REG_ACC_DATA && reg < MCU_REG_ACC_DATA + 6 && !(m->regs[MCU_REG_ACC_CFG] & 1)) {
        return 0;                                             /* accelerometer off */
    }
    if (reg >= MCU_REG_ACC_DATA && reg < MCU_REG_ACC_DATA + 6) {
        uint16_t v = mcu_poses[m->orient & 3][(reg - MCU_REG_ACC_DATA) / 2];
        return (reg - MCU_REG_ACC_DATA) & 1 ? v >> 8 : v;
    }
    if (reg == 0x3D || reg == 0x3E) {                         /* 32768 Hz tick counter */
        uint64_t t = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 32768,
                              NANOSECONDS_PER_SECOND);
        return reg == 0x3D ? t : t >> 8;
    }
    return m->regs[reg];
}

static void mcu_write_reg(CtrMachine *s, uint8_t reg, uint8_t v)
{
    CtrMcu *m = &s->mcu;

    if (reg < 0x02 || (reg >= 0x08 && reg < 0x18) ||
        (reg >= MCU_REG_ACC_DATA && reg < MCU_REG_ACC_DATA + 6)) {
        return;                                               /* read-only */
    }
    if (reg >= MCU_REG_IRQ_MASK && reg < MCU_REG_IRQ_MASK + 4) {
        unsigned sh = (reg - MCU_REG_IRQ_MASK) * 8;
        m->irq_mask = (m->irq_mask & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        return;
    }
    if (reg >= MCU_REG_RTC_SEC && reg <= MCU_REG_RTC_YEAR) {
        struct tm tm;
        int b = from_bcd(v);
        qemu_get_timedate(&tm, m->rtc_offset);
        switch (reg - MCU_REG_RTC_SEC) {
        case 0: tm.tm_sec = b; break;
        case 1: tm.tm_min = b; break;
        case 2: tm.tm_hour = b; break;
        case 3: break;                                        /* day of week: derived */
        case 4: tm.tm_mday = b; break;
        case 5: tm.tm_mon = b - 1; break;
        default: tm.tm_year = 100 + b; break;
        }
        m->rtc_offset = qemu_timedate_diff(&tm);
        return;
    }
    switch (reg) {
    case MCU_REG_SYS_PWR:
        if (v & 1) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        } else if (v & 4) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        return;
    case MCU_REG_LCD_PWR: {
        /* bits: 0 LCDs off, 1 LCDs on, 2/3 bottom backlight off/on, 4/5 top
         * backlight off/on. Nothing to switch here; keep the status register
         * and the "done" IRQs (bits 24..29) truthful. */
        uint8_t st = m->regs[MCU_REG_EX_HW_STAT];
        if (v & 0x01) st &= ~0xE0;
        if (v & 0x02) st |= 0x80;
        if (v & 0x04) st &= ~0x20;
        if (v & 0x08) st |= 0x20;
        if (v & 0x10) st &= ~0x40;
        if (v & 0x20) st |= 0x40;
        m->regs[MCU_REG_EX_HW_STAT] = st;
        mcu_raise(s, (uint32_t)(v & 0x3F) << 24);
        return;
    }
    }
    m->regs[reg] = v;
}

/* registers that take a byte stream instead of auto-incrementing */
static bool mcu_reg_is_stream(uint8_t reg)
{
    return reg == 0x29 || reg == 0x2D || reg == 0x4F || reg == 0x61 || reg == 0x7F;
}

static void mcu_reset(CtrMachine *s)
{
    CtrMcu *m = &s->mcu;

    memset(m->regs, 0, sizeof(m->regs));
    m->reg = 0;
    m->got_reg = false;
    m->irq = m->irq_mask = 0;
    m->regs[0x00] = 0x13;       /* firmware 3.56 */
    m->regs[0x01] = 0x38;
    m->regs[0x09] = 0x20;       /* volume slider */
    m->regs[0x0A] = 28;         /* battery temperature, deg C */
    m->regs[0x0B] = 100;        /* battery percent */
    m->regs[0x0D] = 210;        /* battery voltage, 20 mV units */
    /* shell open, charger plugged, LCDs + both backlights on (Luma screen init) */
    m->regs[MCU_REG_EX_HW_STAT] = 0x02 | 0x08 | 0xE0;
    m->regs[0x08] = 0;          /* 3D slider */
    mcu_set_pose(m, 0);
    if (m->power_held) {
        timer_del(m->power_held);
    }
}

/* --- I2C buses (libn3ds i2c.c) -------------------------------------------- */
#define I2C_CNT_STOP    (1u << 0)
#define I2C_CNT_START   (1u << 1)
#define I2C_CNT_ERROR   (1u << 2)
#define I2C_CNT_ACK     (1u << 4)
#define I2C_CNT_DIR_R   (1u << 5)
#define I2C_CNT_IRQ_EN  (1u << 6)
#define I2C_CNT_EN      (1u << 7)
#define I2C_ADDR_MCU    0x4A        /* 0x25 << 1, bus 2 */

static void i2c_run(CtrMachine *s, CtrI2c *b, uint8_t cnt)
{
    bool mcu = b->bus == 1 && (b->addr & 0xFE) == I2C_ADDR_MCU;
    bool ack = false;

    if (cnt & I2C_CNT_ERROR) {                  /* abort after a NACK */
        b->active = false;
    } else if (!(cnt & I2C_CNT_DIR_R)) {        /* send one byte */
        if (cnt & I2C_CNT_START) {              /* (repeated) start + address byte */
            b->addr = b->data;
            b->active = true;
            mcu = b->bus == 1 && (b->addr & 0xFE) == I2C_ADDR_MCU;
            b->dev_ok = mcu;
            ack = b->dev_ok;
            if (mcu && !(b->addr & 1)) {
                s->mcu.got_reg = false;         /* a write transfer starts with the register */
            }
        } else if (b->active && b->dev_ok && !(b->addr & 1)) {
            if (!s->mcu.got_reg) {
                s->mcu.reg = b->data;
                s->mcu.got_reg = true;
            } else {
                mcu_write_reg(s, s->mcu.reg, b->data);
                if (!mcu_reg_is_stream(s->mcu.reg)) {
                    s->mcu.reg++;
                }
            }
            ack = true;
        }
    } else {                                    /* receive one byte */
        if (b->active && b->dev_ok && (b->addr & 1)) {
            b->data = mcu_read_reg(s, s->mcu.reg);
            if (!mcu_reg_is_stream(s->mcu.reg)) {
                s->mcu.reg++;
            }
        } else {
            b->data = 0xFF;
        }
        ack = cnt & I2C_CNT_ACK;                /* the master's own ACK bit reads back */
    }
    if (cnt & I2C_CNT_STOP) {
        b->active = false;
    }
    /* transfers finish instantly: EN reads back 0, ACK = slave answered */
    b->cnt = (cnt & ~(I2C_CNT_EN | I2C_CNT_ACK)) | (ack ? I2C_CNT_ACK : 0);
    if (cnt & I2C_CNT_IRQ_EN) {
        qemu_irq_pulse(b->irq);
    }
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrI2c *b = opaque;
    switch (addr) {
    case 0: return b->data;
    case 1: return b->cnt;
    case 2: case 3: return b->cntex[addr - 2];
    case 4: case 5: return b->scl[addr - 4];
    }
    return 0;
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrI2c *b = opaque;
    switch (addr) {
    case 0: b->data = v; break;
    case 1:
        if (v & I2C_CNT_EN) {
            i2c_run(ctr, b, v);
        } else {
            b->cnt = v;
        }
        break;
    case 2: b->cntex[0] = v & 0x02; break;      /* bit 0 is the read-only SCL state */
    case 3: b->cntex[1] = v & 0x80; break;
    case 4: case 5: b->scl[addr - 4] = v; break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read, .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* shell open (true) / closed: status bit 1 + the open/close IRQ */
static void mcu_set_shell(CtrMachine *s, bool open)
{
    uint8_t *st = &s->mcu.regs[MCU_REG_EX_HW_STAT];
    if (!!(*st & 0x02) == open) {
        return;
    }
    *st = open ? (*st | 0x02) : (*st & ~0x02);
    mcu_raise(s, open ? MCU_IRQ_SHELL_OPEN : MCU_IRQ_SHELL_CLOSE);
}

/*
 * Runtime MCU state for tests (HMP: qom-set /machine mcu-battery 15):
 *   mcu-battery 0..100, mcu-charging (charger plugged + charging),
 *   mcu-volume 0..63, mcu-shell-open, mcu-orient 0..3 (accelerometer pose),
 *   lcd-backlight (read only: the top screen's backlight register).
 */
static bool mcu_prop_get_charging(Object *obj, Error **errp)
{
    return (ctr->mcu.regs[MCU_REG_EX_HW_STAT] & 0x18) != 0;
}

static void mcu_prop_set_charging(Object *obj, bool on, Error **errp)
{
    uint8_t *st = &ctr->mcu.regs[MCU_REG_EX_HW_STAT];
    *st = on ? (*st | 0x18) : (*st & ~0x18);
}

static bool mcu_prop_get_shell(Object *obj, Error **errp)
{
    return (ctr->mcu.regs[MCU_REG_EX_HW_STAT] & 0x02) != 0;
}

static void mcu_prop_set_shell(Object *obj, bool open, Error **errp)
{
    mcu_set_shell(ctr, open);
}

static void ctr_add_mcu_props(CtrMachine *s, Object *obj)
{
    object_property_add_uint8_ptr(obj, "mcu-battery", &s->mcu.regs[0x0B], OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "mcu-volume", &s->mcu.regs[0x09], OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "mcu-orient", &s->mcu.orient, OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "mcu-charging", mcu_prop_get_charging, mcu_prop_set_charging);
    object_property_add_bool(obj, "mcu-shell-open", mcu_prop_get_shell, mcu_prop_set_shell);
    object_property_add_uint32_ptr(obj, "lcd-backlight", &s->lcd_abl[0], OBJ_PROP_FLAG_READ);
}

/* --- LCD registers (backlight only; libn3ds lcd.h, 3dbrew LCD Registers) -- */
static uint64_t lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x240 || addr == 0xA40) {
        return s->lcd_abl[addr == 0xA40];
    }
    return 0;
}

static void lcd_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x240 || addr == 0xA40) {
        s->lcd_abl[addr == 0xA40] = v & 0x3FF;
        s->lcd_abl_set = true;
        qemu_log_mask(LOG_UNIMP, "nintendo3ds: LCD %s backlight = 0x%x\n",
                      addr == 0xA40 ? "bottom" : "top", (unsigned)(v & 0x3FF));
    }
}

static const MemoryRegionOps lcd_ops = {
    .read = lcd_read, .write = lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- Console input: buttons, circle pad, touch, HOME / POWER -------------- */
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

static void ctr_key_event(CtrMachine *s, int q, bool down)
{
    int bit = qcode_to_hid(q);

    if (bit >= 0) {
        if (down) {
            s->hid_pad &= ~(1u << bit);
        } else {
            s->hid_pad |= (1u << bit);
        }
        return;
    }
    switch (q) {
    /* circle pad: I/K = up/down, J/L = left/right */
    case Q_KEY_CODE_I: s->cpad_y = down ? 1 : (s->cpad_y == 1 ? 0 : s->cpad_y); break;
    case Q_KEY_CODE_K: s->cpad_y = down ? -1 : (s->cpad_y == -1 ? 0 : s->cpad_y); break;
    case Q_KEY_CODE_L: s->cpad_x = down ? 1 : (s->cpad_x == 1 ? 0 : s->cpad_x); break;
    case Q_KEY_CODE_J: s->cpad_x = down ? -1 : (s->cpad_x == -1 ? 0 : s->cpad_x); break;
    case Q_KEY_CODE_H:      /* HOME: MCU IRQ on press and on release */
        mcu_raise(s, down ? MCU_IRQ_HOME_PRESS : MCU_IRQ_HOME_RELEASE);
        break;
    case Q_KEY_CODE_MINUS:  /* volume slider down / up by 8 of 63 */
    case Q_KEY_CODE_EQUAL:
        if (down) {
            int v = s->mcu.regs[0x09] + (q == Q_KEY_CODE_EQUAL ? 8 : -8);
            s->mcu.regs[0x09] = MIN(MAX(v, 0), 0x3F);
            mcu_raise(s, MCU_IRQ_VOL_CHANGE);
        }
        break;
    case Q_KEY_CODE_C:      /* shell: close / open */
        if (down) {
            mcu_set_shell(s, !(s->mcu.regs[MCU_REG_EX_HW_STAT] & 0x02));
        }
        break;
    case Q_KEY_CODE_BRACKET_LEFT:   /* turn the console 90 degrees (accelerometer) */
    case Q_KEY_CODE_BRACKET_RIGHT:
        if (down) {
            mcu_set_pose(&s->mcu, s->mcu.orient + (q == Q_KEY_CODE_BRACKET_RIGHT ? 1 : 3));
        }
        break;
    case Q_KEY_CODE_P:      /* POWER: press IRQ, "held" IRQ after 3 s */
        if (down) {
            if (!timer_pending(s->mcu.power_held)) {
                mcu_raise(s, MCU_IRQ_POWER_PRESS);
                timer_mod(s->mcu.power_held,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 3 * NANOSECONDS_PER_SECOND);
            }
        } else {
            timer_del(s->mcu.power_held);
        }
        break;
    }
}

/*
 * Pointer: the handler takes absolute events (GUI displays, QMP
 * input-send-event; 0..0x7FFF spans the 400x480 window) and relative ones
 * (HMP mouse_move, which can only send deltas) and keeps one cursor position
 * in display pixels. Left button = stylus on the panel.
 */
static void ctr_input_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    CtrMachine *s = ctr;

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = evt->u.key.data;
        ctr_key_event(s, qemu_input_key_value_to_qcode(key->key), key->down);
        break;
    }
    case INPUT_EVENT_KIND_BTN:
        if (evt->u.btn.data->button == INPUT_BUTTON_LEFT) {
            s->ptr_down = evt->u.btn.data->down;
        }
        break;
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *m = evt->u.abs.data;
        if (m->axis == INPUT_AXIS_X) {
            s->ptr_x = m->value * CTR_TOP_W / (INPUT_EVENT_ABS_MAX + 1);
        } else {
            s->ptr_y = m->value * (CTR_SCR_H * 2) / (INPUT_EVENT_ABS_MAX + 1);
        }
        break;
    }
    case INPUT_EVENT_KIND_REL: {
        InputMoveEvent *m = evt->u.rel.data;
        if (m->axis == INPUT_AXIS_X) {
            s->ptr_x = MIN(MAX(s->ptr_x + (int)m->value, 0), CTR_TOP_W - 1);
        } else {
            s->ptr_y = MIN(MAX(s->ptr_y + (int)m->value, 0), CTR_SCR_H * 2 - 1);
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler ctr_input_handler = {
    .name = "3DS buttons + touch panel",
    .mask = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS |
            INPUT_EVENT_MASK_REL,
    .event = ctr_input_event,
};

/* --- CFG11 ---------------------------------------------------------------- */
static uint64_t cfg11_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    switch (addr & ~3) {
    case 0x140:              /* CFG11_GPUPROT */
        return reg_lane(s->gpuprot, addr);
    case 0x1C0:              /* CFG11_SPI_CNT */
        return reg_lane(s->spi_cnt, addr);
    case 0xFFC:              /* CFG11_SOCINFO: bit0 = CTR, bit1 = New3DS */
        return reg_lane(1, addr);
    }
    return 0;
}

static void cfg11_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x1C0) {
        s->spi_cnt = v & 7;
    } else if (addr == 0x140) {
        s->gpuprot = v & 0x10F;
    }
}

static const MemoryRegionOps cfg11_ops = {
    .read = cfg11_read, .write = cfg11_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- PDN: only PDN_GPU_CNT (+0x200) ------------------------------------- */
static uint64_t pdn_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    return (addr & ~3) == 0x200 ? reg_lane(s->gpu_cnt, addr) : 0;
}

static void pdn_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    if (addr == 0x200 && size == 4) {
        s->gpu_cnt = v & 0x1007F;
    }
}

static const MemoryRegionOps pdn_ops = {
    .read = pdn_read, .write = pdn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- GX block: PSC fill, PPF, P3D (software PICA200 model) --------------- */
/* The GPU does DMA by physical address: FCRAM and VRAM are reachable. */
static uint8_t *gx_mem(void *opaque, uint32_t pa, uint32_t len, int write)
{
    CtrMachine *s = opaque;
    if (pa >= CTR_FCRAM_BASE && len <= CTR_FCRAM_SIZE && pa - CTR_FCRAM_BASE <= CTR_FCRAM_SIZE - len) {
        return (uint8_t *)memory_region_get_ram_ptr(&s->fcram) + (pa - CTR_FCRAM_BASE);
    }
    if (pa >= CTR_VRAM_BASE && len <= CTR_VRAM_SIZE && pa - CTR_VRAM_BASE <= CTR_VRAM_SIZE - len) {
        if (write) {
            memory_region_set_dirty(&s->vram, pa - CTR_VRAM_BASE, len);
        }
        return (uint8_t *)memory_region_get_ram_ptr(&s->vram) + (pa - CTR_VRAM_BASE);
    }
    return NULL;
}

static void gx_log(void *opaque, const char *msg)
{
    warn_report("%s", msg);
}

static void gx_reset(CtrMachine *s)
{
    PicaSimHost host = { .mem = gx_mem, .log = gx_log, .opaque = s };
    picasim_init(&s->pica, &host);
    s->gpu_cnt = 0x1007F;       /* Luma initialised the screens: clock on, nothing in reset */
    s->gx_nq = 0;
    s->gpuprot = 0;
}

static uint64_t gx_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    return picasim_read(&s->pica, addr);
}

static void gx_update_irqs(CtrMachine *s)
{
    uint32_t st;
    /* level interrupts follow the status bits (GX +0x34) */
    st = picasim_read(&s->pica, 0x34);
    qemu_set_irq(s->gx_irq[0], (st >> 26) & 1);
    qemu_set_irq(s->gx_irq[1], (st >> 27) & 1);
    qemu_set_irq(s->gx_irq[2], (st >> 30) & 1);
    qemu_set_irq(s->gx_irq[3], (st >> 31) & 1);
}

/*
 * The real GPU works in the background: a started operation finishes some
 * time later and the driver polls (or would take an interrupt). The model
 * itself is synchronous, so the write that starts an operation is held back
 * by roughly the time hardware would take, in virtual time, and then run
 * from a timer. Until then the engine reads as busy (its done bit is clear),
 * which exercises the shim's interrupt-stepped GPU mirror and the driver's
 * polling the way hardware will. The model's work also leaves the vCPU
 * thread. Delays are guesses, not measurements (docs/pica200.md section 9).
 */
static int64_t gx_trigger_delay(uint32_t off, uint32_t v)
{
    if ((off == 0x01C || off == 0x02C) && (v & 1)) {
        return 100 * SCALE_US;          /* PSC fill */
    }
    if (off == 0xC18 && (v & 1)) {
        return 400 * SCALE_US;          /* PPF transfer / texture copy */
    }
    if (off == 0x1000 + 0x23C * 4 || off == 0x1000 + 0x23D * 4) {
        return 1000 * SCALE_US;         /* P3D command list (CMDBUF_JUMPn) */
    }
    return 0;
}

static void gx_run_due(void *opaque)
{
    CtrMachine *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
    int i = 0;
    while (i < s->gx_nq) {
        if (s->gx_q[i].due <= now) {
            uint32_t off = s->gx_q[i].off, val = s->gx_q[i].val;
            memmove(&s->gx_q[i], &s->gx_q[i + 1], (s->gx_nq - i - 1) * sizeof s->gx_q[0]);
            s->gx_nq--;
            picasim_write(&s->pica, off, val);
            gx_update_irqs(s);
            i = 0;
            continue;
        }
        if (s->gx_q[i].due < next) {
            next = s->gx_q[i].due;
        }
        i++;
    }
    if (s->gx_nq) {
        timer_mod(s->gx_timer, next);
    }
}

static void gx_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    int64_t d;
    if (!(s->gpu_cnt & 0x10000)) {
        return;                 /* unclocked */
    }
    d = gx_trigger_delay(addr, v);
    if (d && s->gx_nq < (int)ARRAY_SIZE(s->gx_q)) {
        int64_t due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + d;
        s->gx_q[s->gx_nq].off = addr;
        s->gx_q[s->gx_nq].val = v;
        s->gx_q[s->gx_nq].due = due;
        s->gx_nq++;
        if (!timer_pending(s->gx_timer) || timer_expire_time_ns(s->gx_timer) > due) {
            timer_mod(s->gx_timer, due);
        }
        return;
    }
    picasim_write(&s->pica, addr, v);
    gx_update_irqs(s);
}

static const MemoryRegionOps gx_ops = {
    .read = gx_read, .write = gx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- PDC (two display controllers, 0x100 bytes each) --------------------- */
static void pdc_update_irq(CtrMachine *s, int n)
{
    CtrPdc *p = &s->pdc_regs[n];
    uint32_t masked = (p->regs[PDC_CNT] >> 8) & 7;   /* NO_IRQ_H / _V / _ERR */

    qemu_set_irq(s->pdc_irq[n], (p->irq_stat & ~masked) != 0);
}

static bool pdc_wants_hblank(CtrMachine *s)
{
    for (int n = 0; n < 2; n++) {
        uint32_t cnt = s->pdc_regs[n].regs[PDC_CNT];
        if ((cnt & PDC_CNT_EN) && !(cnt & PDC_CNT_NO_IRQ_H)) {
            return true;
        }
    }
    return false;
}

static uint64_t pdc_read(void *opaque, hwaddr addr, unsigned size)
{
    CtrMachine *s = opaque;
    unsigned n = addr >> 8;
    CtrPdc *p;

    if (n >= 2) {
        return 0;
    }
    p = &s->pdc_regs[n];
    switch ((addr & 0xFF) / 4) {
    case PDC_SELECT:         /* next fb, current fb, IRQ status */
        return (p->regs[PDC_SELECT] & 1) | (p->cur_fb << 4) | (p->irq_stat << 16);
    case PDC_V_COUNT:
        if (!(p->regs[PDC_CNT] & PDC_CNT_EN)) {
            return 0;
        }
        return ((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->vblank_stamp) / PDC_LINE_NS)
               % PDC_LINES;
    }
    return p->regs[(addr & 0xFF) / 4];
}

static void pdc_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CtrMachine *s = opaque;
    unsigned n = addr >> 8;
    CtrPdc *p;

    if (n >= 2) {
        return;
    }
    p = &s->pdc_regs[n];
    switch ((addr & 0xFF) / 4) {
    case PDC_SELECT:
        p->irq_stat &= ~((v >> 16) & 7);        /* bits 16..18 acknowledge H / V / error */
        p->regs[PDC_SELECT] = v & 1;            /* taken over at the next VBlank */
        break;
    case PDC_V_COUNT:
        return;                                 /* read-only */
    default:
        p->regs[(addr & 0xFF) / 4] = v;
        break;
    }
    pdc_update_irq(s, n);
    if (pdc_wants_hblank(s) && !timer_pending(s->hblank)) {
        timer_mod(s->hblank, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PDC_LINE_NS);
    }
}

/* 60 Hz VBlank: latch the status bit and the framebuffer select of every
 * enabled PDC; GIC 42/43 follow the unmasked status bits */
static void ctr_vblank(void *opaque)
{
    CtrMachine *s = opaque;

    s->vblank_stamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (int n = 0; n < 2; n++) {
        CtrPdc *p = &s->pdc_regs[n];
        if (p->regs[PDC_CNT] & PDC_CNT_EN) {
            p->irq_stat |= PDC_IRQ_V;
            p->cur_fb = p->regs[PDC_SELECT] & 1;
            pdc_update_irq(s, n);
        }
    }
    timer_mod(s->vblank, s->vblank_stamp + NANOSECONDS_PER_SECOND / 60);
}

/* per-scanline IRQ; the timer only runs while a guest has H IRQs unmasked
 * (Luma hands over with them masked: PDC_CNT 0x00010501) */
static void ctr_hblank(void *opaque)
{
    CtrMachine *s = opaque;

    if (!pdc_wants_hblank(s)) {
        return;
    }
    for (int n = 0; n < 2; n++) {
        CtrPdc *p = &s->pdc_regs[n];
        if ((p->regs[PDC_CNT] & PDC_CNT_EN) && !(p->regs[PDC_CNT] & PDC_CNT_NO_IRQ_H)) {
            p->irq_stat |= PDC_IRQ_H;
            pdc_update_irq(s, n);
        }
    }
    timer_mod(s->hblank, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PDC_LINE_NS);
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
    /* backlight: full brightness until the guest programs it; then level
       0x8F (ios3ds' brightest) = full, 0 = dark (docs/mcu.md) */
    uint32_t bl = s->lcd_abl_set ? MIN(s->lcd_abl[n] * 256 / 0x8F, 256) : 256;
    if (!(s->mcu.regs[MCU_REG_EX_HW_STAT] & (n ? 0x20 : 0x40))) {
        bl = 0;      /* backlight switched off through the MCU (reg 0x22) */
    }
    CtrPdc *p = &s->pdc_regs[n];
    uint32_t fb = p->cur_fb ? p->regs[PDC_FB_A1] : p->regs[PDC_FB_A0];
    unsigned fmt = p->regs[PDC_FORMAT] & 7;
    unsigned bpp = (fmt == 0) ? 4 : (fmt == 1) ? 3 : 2;
    uint32_t stride = p->regs[PDC_STRIDE] ? p->regs[PDC_STRIDE] : CTR_SCR_H * bpp;
    g_autofree uint8_t *buf = NULL;
    size_t len = (size_t)stride * width;

    if (!fb || !(p->regs[PDC_CNT] & PDC_CNT_EN)) {
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
            if (bl < 256) {
                r = r * bl >> 8; g = g * bl >> 8; b = b * bl >> 8;
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
            pd->regs[PDC_CNT] = 0x00010501;   /* on, output on, H + error IRQs masked */
        }
    }
}

/* Peripheral state as Luma3DS leaves it when it jumps to a payload */
static void ctr_reset_io(void *opaque)
{
    CtrMachine *s = opaque;

    s->hid_pad = 0xFFF;
    s->sdmmcctl = SDMMCCTL_CARD_TMIO1_SEL;      /* 0x200: slot on TMIO1, powered */
    s->spi_cnt = 0;
    s->irq9_ie = s->irq9_if = 0;
    for (int i = 0; i < 4; i++) {
        s->tmr9[i].reload = s->tmr9[i].cnt = 0;
        s->tmr9[i].counter = 0;
        timer_del(s->tmr9[i].t);
    }
    for (int i = 0; i < 3; i++) {
        CtrNspi *n = &s->nspi[i];
        n->cnt = n->blklen = n->autopoll = n->int_mask = n->int_stat = n->left = 0;
        n->cs_low = false;
        CtrI2c *b = &s->i2c[i];
        b->data = b->cnt = 0;
        b->cntex[0] = b->cntex[1] = b->scl[0] = b->scl[1] = 0;
        b->active = b->dev_ok = false;
    }
    codec_reset(&s->codec);
    mcu_reset(s);
    for (int n = 0; n < 2; n++) {
        s->pdc_regs[n].irq_stat = 0;
        s->pdc_regs[n].cur_fb = false;
    }
    s->ptr_down = false;
    s->cpad_x = s->cpad_y = 0;
    gx_reset(s);
}

static void ctr_reset_arm9(void *opaque)
{
    CtrMachine *s = opaque;
    CPUARMState *env = &s->cpu9->env;

    cpu_reset(CPU(s->cpu9));
    env->regs[0] = s->screen_init ? 2 : 1;
    env->regs[1] = s->argv_addr;
    env->regs[2] = 0x4BEEF;                /* boot9strap magic */
    /* Luma arm9 start.s/chainloader: high vectors, ITCM + DTCM enabled,
     * caches and MPU off, DTCM region register 0xFFF0000A */
    env->cp15.sctlr_el[1] |= SCTLR_V | (1u << 18) | (1u << 16);
    env->cp15.sctlr_el[1] &= ~((1u << 12) | (1u << 2) | (1u << 0));
    arm_rebuild_hflags(env);
    cpu_set_pc(CPU(s->cpu9), s->arm9_entry);
}

static void ctr_reset_arm11(void *opaque)
{
    CtrMachine *s = opaque;

    cpu_reset(CPU(s->cpu11));
    /* Luma arm11 start.s: SCTLR and ACTLR at their reset defaults (MMU and
     * caches off; ACTLR 0xF comes from reset_auxcr set in ctr_init) */
    s->cpu11->env.cp15.sctlr_el[1] = 0x00054078;
    arm_rebuild_hflags(&s->cpu11->env);
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

    /* ---- address spaces ---- */
    memory_region_init(&s->arm9_root, NULL, "arm9-memory", UINT64_MAX);
    address_space_init(&s->arm9_as, &s->arm9_root, "arm9");

    /* ---- CPUs (one cluster each: different CPU types) ---- */
    object_initialize_child(OBJECT(machine), "arm11-cluster", &s->cluster11, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster11), "cluster-id", 0);
    cpuobj = object_new(ARM_CPU_TYPE_NAME("arm11mpcore"));
    object_property_add_child(OBJECT(&s->cluster11), "cpu0", cpuobj);
    ARM_CPU(cpuobj)->reset_auxcr = 0xF;      /* ACTLR as Luma's ARM11 stub sets it */
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
    /*
     * The O3DS SoC has two cores. Core 1 stays parked in boot11 for us and is
     * not instantiated, but the SCU/GIC are sized for two: SPI targets then
     * reset to "no CPU" as on hardware (a one-CPU QEMU GIC routes everything
     * to CPU 0 implicitly) and a guest must program GICD_ITARGETSR.
     */
    qdev_prop_set_uint32(dev, "num-cpu", 2);
    qdev_prop_set_uint32(dev, "num-irq", 128);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sys, CTR_MPCORE_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu11), ARM_CPU_IRQ));
    s->gic = dev;
    s->pdc_irq[0] = qdev_get_gpio_in(dev, 42 - 32);
    s->pdc_irq[1] = qdev_get_gpio_in(dev, 43 - 32);
    s->gx_irq[0] = qdev_get_gpio_in(dev, 40 - 32);
    s->gx_irq[1] = qdev_get_gpio_in(dev, 41 - 32);
    s->gx_irq[2] = qdev_get_gpio_in(dev, 44 - 32);
    s->gx_irq[3] = qdev_get_gpio_in(dev, 45 - 32);
    s->mcu_irq = qdev_get_gpio_in(dev, 113 - 32);
    s->hblank = timer_new_ns(QEMU_CLOCK_VIRTUAL, ctr_hblank, s);
    s->gx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gx_run_due, s);
    s->mcu.power_held = timer_new_ns(QEMU_CLOCK_VIRTUAL, mcu_power_held, s);
    s->vblank = timer_new_ns(QEMU_CLOCK_VIRTUAL, ctr_vblank, s);
    timer_mod(s->vblank, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);

    /* ---- shared IO 0x10100000-0x101FFFFF (visible to both CPUs) ---- */
    memory_region_init(&s->io_shared, NULL, "ctr.io-shared", 0x100000);
    memory_region_init_io(&s->hid, NULL, &hid_ops, s, "ctr.hid", 0x8);
    memory_region_add_subregion(&s->io_shared, CTR_HID_BASE - CTR_IO_SHARED_BASE, &s->hid);
    memory_region_init_io(&s->cfg11, NULL, &cfg11_ops, s, "ctr.cfg11", 0x1000);
    memory_region_add_subregion(&s->io_shared, CTR_CFG11_BASE - CTR_IO_SHARED_BASE, &s->cfg11);
    memory_region_init_io(&s->pdn, NULL, &pdn_ops, s, "ctr.pdn", 0x1000);
    memory_region_add_subregion(&s->io_shared, 0x10141000 - CTR_IO_SHARED_BASE, &s->pdn);
    /* CSND sound block: plays from FCRAM, records the mix (ctr_csnd.c) */
    ctr_csnd_init(&s->io_shared, 0x10103000 - CTR_IO_SHARED_BASE, &s->fcram, CTR_FCRAM_BASE);
    {
        /* libn3ds spi.h / i2c.h bases and interrupt.h IRQ ids, bus 1..3 */
        static const hwaddr nspi_base[3] = { 0x10160800, 0x10142800, 0x10143800 };
        static const int nspi_irq[3] = { 87, 36, 86 };
        static const hwaddr i2c_base[3] = { 0x10161000, 0x10144000, 0x10148000 };
        static const int i2c_irq[3] = { 84, 85, 92 };
        for (int i = 0; i < 3; i++) {
            g_autofree char *nn = g_strdup_printf("ctr.nspi%d", i + 1);
            g_autofree char *in = g_strdup_printf("ctr.i2c%d", i + 1);
            s->nspi[i].bus = i;
            s->nspi[i].irq = qdev_get_gpio_in(dev, nspi_irq[i] - 32);
            memory_region_init_io(&s->nspi[i].iomem, NULL, &nspi_ops, &s->nspi[i], nn, 0x20);
            memory_region_add_subregion(&s->io_shared, nspi_base[i] - CTR_IO_SHARED_BASE,
                                        &s->nspi[i].iomem);
            s->i2c[i].bus = i;
            s->i2c[i].irq = qdev_get_gpio_in(dev, i2c_irq[i] - 32);
            memory_region_init_io(&s->i2c[i].iomem, NULL, &i2c_ops, &s->i2c[i], in, 0x6);
            memory_region_add_subregion(&s->io_shared, i2c_base[i] - CTR_IO_SHARED_BASE,
                                        &s->i2c[i].iomem);
        }
    }
    memory_region_add_subregion(sys, CTR_IO_SHARED_BASE, &s->io_shared);
    memory_region_init_alias(&s->io_shared9, NULL, "ctr.io-shared9", &s->io_shared, 0, 0x100000);
    memory_region_add_subregion(&s->arm9_root, CTR_IO_SHARED_BASE, &s->io_shared9);
    ctr_add_unimp(&s->io_shared, "ctr.io-shared.unimp", 0, 0x100000);

    /* ---- ARM9-only IO ---- */
    memory_region_init_io(&s->cfg9, NULL, &cfg9_ops, s, "ctr.cfg9", 0x1000);
    memory_region_add_subregion(&s->arm9_root, CTR_CFG9_BASE, &s->cfg9);
    s->irq9_in = qemu_allocate_irqs(irq9_set, s, IRQ9_LINES);
    memory_region_init_io(&s->irq9, NULL, &irq9_ops, s, "ctr.irq9", 0x8);
    memory_region_add_subregion(&s->arm9_root, CTR_IRQ9_BASE, &s->irq9);
    for (int i = 0; i < 4; i++) {
        s->tmr9[i].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, tmr9_tick, &s->tmr9[i]);
    }
    memory_region_init_io(&s->timer9, NULL, &tmr9_ops, s, "ctr.timer9", 0x10);
    memory_region_add_subregion(&s->arm9_root, CTR_TIMER9_BASE, &s->timer9);
    dev = qdev_new(TYPE_CTR_TMIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s->irq9_in[IRQ9_TMIO1]);
    memory_region_add_subregion(&s->arm9_root, CTR_TMIO1_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
    {
        DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
        if (dinfo) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);
            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo), &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"), &error_fatal);
            s->sdcard = card;
        } else {
            warn_report("nintendo3ds: no SD card (-drive if=sd,...); SD slot empty");
        }
    }
    ctr_add_unimp(&s->arm9_root, "ctr.io9.unimp", CTR_IO9_BASE, 0x100000);

    /* ---- ARM11-only IO ---- */
    memory_region_init_io(&s->pdc, NULL, &pdc_ops, s, "ctr.pdc", 0x200);
    memory_region_add_subregion(sys, CTR_PDC0_BASE, &s->pdc);
    /* the rest of the GX block; the PDC registers above take precedence */
    memory_region_init_io(&s->gx, NULL, &gx_ops, s, "ctr.gx", 0x2000);
    memory_region_add_subregion_overlap(sys, 0x10400000, &s->gx, -1);
    memory_region_init_io(&s->lcd, NULL, &lcd_ops, s, "ctr.lcd", 0x1000);
    memory_region_add_subregion(sys, 0x10202000, &s->lcd);
    s->lcd_abl[0] = s->lcd_abl[1] = 0x5F;          /* Luma's default brightness preset */
    ctr_add_unimp(sys, "ctr.io11.unimp", CTR_IO11_BASE, 0x400000);

    /* ---- display + input ---- */
    s->con = graphic_console_init(NULL, 0, &ctr_gfx_ops, s);
    qemu_console_resize(s->con, CTR_TOP_W, CTR_SCR_H * 2);
    qemu_input_handler_register(NULL, &ctr_input_handler);
    ctr_add_mcu_props(s, OBJECT(machine));

    /* ---- payload ---- */
    if (!machine->kernel_filename) {
        error_report("nintendo3ds: use -kernel <payload.firm>");
        exit(1);
    }
    ctr_load_firm(s, machine->kernel_filename);
    qemu_register_reset(ctr_reset_io, s);
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
