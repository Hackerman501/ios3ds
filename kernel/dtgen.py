"""Generate the ios3ds device tree from the n72ap template (EmbeddedDeviceTrees-617.53.1).

Everything here only fills the zeroed slots iBoot would fill or renames spare
memory-map slots, so the tree keeps its exact size and property order. Node
removal / 3DS nodes come in once the replacement drivers exist (M2+).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
from fwlib import appledt  # noqa: E402

CPU_HZ = 268111856           # O3DS ARM11 (libn3ds timer.h)
TIMER_HZ = (CPU_HZ // 2) // 128   # MPCore watchdog/private timer at PERIPHCLK/128 (kernel/shim/shim.S PRESCALE)

# Carve-outs outside XNU's RAM (iBoot puts these at the top of DRAM):
PRAM = (0x1FFA0000, 0x10000)   # AXI WRAM: persistent panic log ('OOPS'), ARM9-readable
VRAM = (0x18000000, 0x300000)  # 3DS VRAM bank 0 (Luma's framebuffers live at 0x18300000+)
# "Fake register pages": RAM in AXI WRAM that stands in for S5L8720 register
# blocks the kernel insists on mapping/writing early (the shim emulates the
# parts that matter). One 4 KiB page each.
FAKEIO_BASE = 0x1FFB0000
FAKEIO_PAGES = 32
FAKEIO = ["timer", "vic", "edgeic", "gpio", "power", "clkrstgen", "wdt", "uart0", "clcd"]
# S5L8720 peripherals that have no 3DS counterpart yet, or that ios3ds
# replaces with its own drivers: removed so their kexts never match.
DELETE = [
    "arm-io/dmac0", "arm-io/dmac1", "arm-io/sha1", "arm-io/usb-complex", "arm-io/amc",
    "arm-io/flash-controller0", "arm-io/aes", "arm-io/sdio",
    "arm-io/h264decoder", "arm-io/scaler", "arm-io/tv-out", "arm-io/mbx", "arm-io/mpvd",
    "arm-io/prng", "arm-io/spi0", "arm-io/spi1", "arm-io/spi4", "arm-io/otgphyctrl",
    "arm-io/i2c0", "arm-io/i2c1", "arm-io/i2s0", "arm-io/uart0/iap", "arm-io/uart1",
    "arm-io/pke", "arm-io/mipi-dsim", "arm-io/swi", "arm-io/xmc",
    "buttons", "dock", "charger", "backlight",
]
# audio (docs/audio.md): keep dmac0/i2c0/i2s0 on fake pages, see tools/ios3ds-audio/dt_audio.py
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "ios3ds-audio"))
import dt_audio  # noqa: E402
DELETE = [p for p in DELETE if p not in dt_audio.KEEP] + dt_audio.DELETE
FAKEIO = FAKEIO + dt_audio.FAKEIO
# MCU (docs/mcu.md): keep the accelerometer node, see tools/ios3ds-mcu/dt_mcu.py
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "ios3ds-mcu"))
import dt_mcu  # noqa: E402
DELETE = [p for p in DELETE if p not in dt_mcu.KEEP]
# arm-io 'ranges' rewritten for the 3DS:
#   child 0x00000000.. -> FAKEIO_BASE (fake S5L register pages)
#   child 0x10000000.. -> PA 0x10000000 (identity: real 3DS IO, VRAM, AXI WRAM,
#                         MPCore private region) for ios3ds drivers.
# pexpert only ever uses the FIRST range's parent (child 0x17FB0000 was mapped
# at 0x38000000+child), IOKit checks every range: both agree with this layout.
ARMIO_RANGES = [(0x00000000, FAKEIO_BASE, FAKEIO_PAGES * 0x1000),
                (0x10000000, 0x10000000, 0x10000000)]
def armio_child(pa):
    assert FAKEIO_BASE <= pa < FAKEIO_BASE + FAKEIO_PAGES * 0x1000
    return pa - FAKEIO_BASE


def _u32(v):
    return struct.pack("<I", v)


def _fill_str(node, name, text):
    old = node.get(name)
    if old is None:
        return
    raw = text.encode()[: len(old) - 1]
    node.set(name, raw + b"\0" * (len(old) - len(raw)))


def _set_same_size(node, name, value):
    old = node.get(name)
    if old is None:
        raise KeyError("template lacks %s" % name)
    if len(old) != len(value):
        raise ValueError("%s: size %d != template %d" % (name, len(value), len(old)))
    node.set(name, value)


CLCD_INIT = {0x00: 0x3, 0x04: 0x10, 0x10: 0x80, 0x20: 0x00200700,
             0x24: 0x18000000, 0x28: 320, 0x30: (320 << 16) | 480}
PAGE_MAP = {}   # node name -> list of fake page indices (filled by generate())

PL192_ID = bytes([0x92, 0, 0, 0, 0x11, 0, 0, 0, 0x04, 0, 0, 0, 0x00, 0, 0, 0,     # PeriphID0-3
                  0x0D, 0, 0, 0, 0xF0, 0, 0, 0, 0x05, 0, 0, 0, 0xB1, 0, 0, 0])    # PCellID0-3


def fakeio_image():
    """Initial contents of the fake register pages (loaded by the ARM9 at
    FAKEIO_BASE). Values a driver checks at probe/start time go here."""
    img = bytearray(FAKEIO_PAGES * 0x1000)
    for page in PAGE_MAP.get("vic", []):        # both PL192s: PrimeCell IDs
        img[page * 0x1000 + 0xFE0: page * 0x1000 + 0x1000] = PL192_ID
    # S5L8720 CLCD as iBoot leaves it (window 1 on, 320x480 32bpp at /vram);
    # AppleM2CLCD::create_default_fb_surface reads these (0x807a9ff8..)
    if "clcd" in PAGE_MAP:
        base = PAGE_MAP["clcd"][0] * 0x1000
        for off, val in CLCD_INIT.items():
            struct.pack_into("<I", img, base + off, val)
    return bytes(img)


def generate(template, lay, phys_base, virt_base, mem_size, kernel_len, debug_enabled=True):
    root = appledt.parse(template)
    chosen = root.find("chosen")
    _set_same_size(chosen, "debug-enabled", _u32(1 if debug_enabled else 0))
    _fill_str(chosen, "firmware-version", "ios3ds-loader")
    _fill_str(root, "serial-number", "IOS3DS000001")
    _fill_str(root, "mlb-serial-number", "IOS3DS000001")
    _fill_str(root, "model-number", "MC086")
    _fill_str(root, "region-info", "LL/A")
    _fill_str(root, "platform-name", "s5l8720x")
    _fill_str(root, "config-number", "ios3ds")

    # 'secure-root-prefix = md' makes an md root wait for a SecureRoot
    # callback (img3-signed restore ramdisk check) that no driver here provides
    root.delete("secure-root-prefix")

    mm = chosen.find("memory-map")
    entries = [
        ("DeviceTree", lay["devtree_pa"], lay["bootargs_pa"] - lay["devtree_pa"]),
        ("BootArgs", lay["bootargs_pa"], 0x1000),
    ]
    if lay.get("ramdisk_len"):
        # md0: IOFindBSDRoot mdevadd()s this; its I/O never touches the
        # address (memdev strategy is redirected to the ARM9 block server)
        entries.append(("RAMDisk", phys_base, lay["ramdisk_len"]))
    slots = [p for p in mm.props if p[0].startswith("MemoryMapReserved-")]
    slots.sort(key=lambda p: int(p[0].split("-")[1]))
    for (name, pa, ln), slot in zip(entries, slots):
        slot[0] = name
        slot[1] = struct.pack("<II", pa, ln)

    _set_same_size(root.find("memory"), "reg", struct.pack("<II", phys_base, mem_size))
    # pexpert ml_io_map()s these and reads them early (pram: panic-log magic)
    _set_same_size(root.find("pram"), "reg", struct.pack("<II", *PRAM))
    _set_same_size(root.find("vram"), "reg", struct.pack("<II", *VRAM))

    for path in DELETE:
        parent_path, _, leaf = path.rpartition("/")
        parent = root.find(parent_path) if parent_path else root
        child = parent.find(leaf) if parent else None
        if child is None:
            raise KeyError("dtgen: no node /%s to delete" % path)
        parent.children.remove(child)

    dt_audio.apply(root)
    dt_mcu.apply(root)

    # its target (mipi-dsim/lcd, Pinot panel) is deleted; AppleM2CLCD copes with NULL
    clcd = root.find("arm-io/clcd")
    if clcd is not None:
        clcd.delete("function-lcd_enable")

    armio = root.find("arm-io")
    if armio is not None:
        armio.set("ranges", b"".join(struct.pack("<III", *r) for r in ARMIO_RANGES))

    # every reg window of these nodes -> its own fake page(s); identical
    # original windows (e.g. gpio's 2nd window == power) share pages
    fake = {}
    next_page = 0
    PAGE_MAP.clear()
    for name in FAKEIO:
        node = root.find("arm-io/" + name)
        if node is None:
            raise KeyError("dtgen: no node /arm-io/%s to back with a fake page" % name)
        reg = node.get("reg")
        out = b""
        for i in range(0, len(reg), 8):
            child, size = struct.unpack_from("<II", reg, i)
            if child not in fake:
                fake[child] = next_page
                next_page += (size + 0xFFF) // 0x1000
            PAGE_MAP.setdefault(name, []).extend(range(fake[child], fake[child] + (size + 0xFFF) // 0x1000))
            out += struct.pack("<II", armio_child(FAKEIO_BASE + fake[child] * 0x1000), size)
        _set_same_size(node, "reg", out)
    if next_page > FAKEIO_PAGES:
        raise AssertionError("fake IO pages overflow")

    cpu = root.find("cpus/cpu0")
    for name, hz in (("clock-frequency", CPU_HZ), ("bus-frequency", CPU_HZ // 2),
                     ("peripheral-frequency", CPU_HZ // 4), ("memory-frequency", CPU_HZ),
                     ("fixed-frequency", 24000000), ("timebase-frequency", TIMER_HZ)):
        _set_same_size(cpu, name, _u32(hz))

    return appledt.serialise(root)
