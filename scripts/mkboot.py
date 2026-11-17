#!/usr/bin/env python3
"""Turn the staged 4.2.1 kernelcache + device tree into what the 3DS loader boots.

  scripts/mkboot.py [--staging build/staging] [--out build/sd/ios3ds] [--cmdline "..."]

Writes:
  kernel.bin   flat physical image of the kernelcache, loaded at kernel_pa
               (segments placed at vmaddr - virtBase + physBase, bss zeroed),
               with the patches from kernel/patches.json applied
  devtree.bin  device tree generated from the n72ap template (kernel/dtgen.py)
  boot.cfg     loader configuration read by the ARM9 (layout + boot-args)

Memory layout (all inside FCRAM, see docs/m1-design.md):
  0x20000000                 physBase (VA 0x80000000)
  0x20001000                 kernelcache (VA 0x80001000 ..)
  kernel_end (4 KiB aligned) device tree
  next 4 KiB                 boot_args
  next 4 KiB                 XNU log ring (256 KiB)
  1 MiB aligned              topOfKernelData (kernel builds its page tables here)
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "kernel"))
from fwlib.macho import MachO  # noqa: E402

PHYS_BASE = 0x20000000
VIRT_BASE = 0x80000000
MEM_SIZE = 0x08000000
RING_BUF = 0x40000            # power of two; ring header (16 bytes) precedes it
RING_SIZE = RING_BUF + 16
SHIM_PA = 0x20840000          # fixed: the shim is linked at VA 0x80840000 (boot/common/ios3ds.h)
SHIM_SIZE = 0x10000
DEFAULT_CMDLINE = "rd=md0 debug=0x8 serial=3 amfi_allow_any_signature=1 cs_enforcement_disable=1 amfi_unrestrict_task_for_pid=1"


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def flat_kernel(mo, blob):
    segs = [s for s in mo.segments if s["vmsize"] > 0]
    lo = min(s["vmaddr"] for s in segs)
    hi = max(s["vmaddr"] + s["vmsize"] for s in segs)
    img = bytearray(hi - lo)
    for s in segs:
        n = min(s["filesize"], s["vmsize"])
        off = s["vmaddr"] - lo
        img[off:off + n] = blob[s["fileoff"]:s["fileoff"] + n]
    return lo, img


def layout(kernel_va, kernel_len, dt_len):
    kernel_pa = kernel_va - VIRT_BASE + PHYS_BASE
    devtree_pa = align(kernel_pa + kernel_len, 0x1000)
    bootargs_pa = align(devtree_pa + dt_len, 0x1000)
    shim_pa = SHIM_PA
    if bootargs_pa + 0x1000 > shim_pa:
        raise SystemExit("kernel + device tree + boot_args overlap the shim at %#x" % SHIM_PA)
    ring_pa = shim_pa + SHIM_SIZE
    top_pa = align(ring_pa + RING_SIZE, 0x100000)
    return {"kernel_pa": kernel_pa, "devtree_pa": devtree_pa, "bootargs_pa": bootargs_pa,
            "shim_pa": shim_pa, "ring_pa": ring_pa, "ring_size": RING_SIZE, "top_pa": top_pa}


def build_shim(outdir):
    """Build kernel/shim (entry.S, blk.c, the input and GPU services) linked at its fixed kernel VA."""
    import subprocess
    import tempfile
    cross = subprocess.run([os.path.join(ROOT, "scripts", "setup-toolchain.sh")],
                           capture_output=True, text=True).stdout
    m = __import__("re").search(r"cross compiler: (\S+)gcc ", cross)
    if not m:
        raise SystemExit("no arm-none-eabi toolchain (scripts/setup-toolchain.sh)")
    gcc, objcopy = m.group(1) + "gcc", m.group(1) + "objcopy"
    sd = os.path.join(ROOT, "kernel", "shim")
    flags = ["-mcpu=mpcore", "-marm", "-mfloat-abi=soft", "-O2", "-ffreestanding", "-fno-builtin",
             "-fno-tree-loop-distribute-patterns", "-nostdlib", "-Wall", "-Wextra",
             # iOS' ABI lets callees clobber r9; keep our C code off it so calls into the kernel are safe
             "-ffixed-r9",
             "-I" + os.path.join(ROOT, "boot", "common"),
             "-I" + os.path.join(ROOT, "tools", "ios3ds-pica", "include")]
    srcs = [os.path.join(sd, "entry.S"), os.path.join(sd, "blk.c"), os.path.join(sd, "gpu_glue.c"),
            os.path.join(ROOT, "tools", "ios3ds-input", "shim", "input.c"),
            os.path.join(ROOT, "tools", "ios3ds-wifi", "shim", "net.c"),
            os.path.join(ROOT, "tools", "ios3ds-mcu", "shim", "mcu.c")]
    srcs += sorted(__import__("glob").glob(os.path.join(sd, "audio*.[cS]")))     # docs/audio.md
    # the GPU part: link.ld places every object named *pica*.o at GPU_PART_VA (gpu.bin)
    srcs += [os.path.join(sd, "pica.c"),
             os.path.join(ROOT, "tools", "ios3ds-pica", "src", "pica_drv.c"),
             os.path.join(ROOT, "tools", "ios3ds-pica", "src", "pica_mirror.c"),
             os.path.join(ROOT, "tools", "ios3ds-pica", "src", "pica_diag.c")]
    with tempfile.TemporaryDirectory() as td:
        elf = os.path.join(td, "shim.elf")
        objs = []
        for s in srcs:
            o = os.path.join(td, os.path.splitext(os.path.basename(s))[0] + ".o")
            subprocess.run([gcc] + flags + ["-c", s, "-o", o], check=True)
            objs.append(o)
        subprocess.run([gcc] + flags + ["-T", os.path.join(sd, "link.ld")] + objs +
                       ["-o", elf, "-Wl,--no-warn-rwx-segments"], check=True)
        out = os.path.join(outdir, "shim.bin")
        subprocess.run([objcopy, "-O", "binary", "-R", ".gpu", elf, out], check=True)
        gpart = os.path.join(td, "gpu_part.bin")
        subprocess.run([objcopy, "-O", "binary", "-j", ".gpu", elf, gpart], check=True)
        gpu = open(gpart, "rb").read()
    blob = open(out, "rb").read()
    if len(blob) > 0x6000 or blob[4:8] != b"SHIM":
        raise SystemExit("bad shim build (%d bytes)" % len(blob))
    build_gpu_bin(outdir, gpu)
    return blob


GPU_PART_SIZE = 0x8000        # boot/common/gpu_mirror.h: GPU part at 0x208B0000, blob at 0x208B8000
GPU_BIN_PA = 0x208B0000


def build_gpu_bin(outdir, part):
    """gpu.bin = the shim's GPU part (padded to 32 KiB) + the mirror's command lists."""
    import subprocess
    if len(part) > GPU_PART_SIZE or struct.unpack_from("<I", part, 0)[0] != 0x30555047:
        raise SystemExit("bad shim GPU part (%d bytes)" % len(part))
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "tools", "ios3ds-pica"), "mirror"], check=True)
    mirror = open(os.path.join(ROOT, "build", "pica-host", "gpumirror.bin"), "rb").read()
    with open(os.path.join(outdir, "gpu.bin"), "wb") as f:
        f.write(part + bytes(GPU_PART_SIZE - len(part)) + mirror)


def configure_shim(outdir, cfg):
    """Patch the shim's config words (+0x70 vic0 PA, +0x74 clcd PA)."""
    p = os.path.join(outdir, "shim.bin")
    blob = bytearray(open(p, "rb").read())
    struct.pack_into("<II", blob, 0x70, cfg.get("vic0", 0), cfg.get("clcd", 0))
    i = blob.find(b"AUD0cfg!")      # aud_cfg_t (kernel/shim/audio.h): fake PL080 / I2S pages
    if i >= 0:
        struct.pack_into("<II", blob, i + 8, cfg.get("dmac0", 0), cfg.get("i2s0", 0))
    open(p, "wb").write(blob)


def use_prebuilt_shim(outdir, prebuilt):
    """Copy a released shim.bin + gpu.bin (as `--shim-only` writes them) instead of compiling."""
    import shutil
    for fn in ("shim.bin", "gpu.bin"):
        shutil.copyfile(os.path.join(prebuilt, fn), os.path.join(outdir, fn))
    blob = open(os.path.join(outdir, "shim.bin"), "rb").read()
    if len(blob) > 0x6000 or blob[4:8] != b"SHIM":
        raise SystemExit("bad prebuilt shim.bin (%d bytes)" % len(blob))
    return blob


def build(staging, outdir, cmdline=None, patches=True, verbose=True,
          prebuilt=None, hfsplus=None, rootfs_add=None):
    """prebuilt: directory with shim.bin + gpu.bin (no cross compiler needed);
    hfsplus / rootfs_add: passed on to mkrootfs.build."""
    import dtgen
    import patcher

    blob = open(os.path.join(staging, "kernelcache.macho"), "rb").read()
    mo = MachO(blob)
    kva, kimg = flat_kernel(mo, blob)
    template = open(os.path.join(staging, "devicetree.bin"), "rb").read()
    os.makedirs(outdir, exist_ok=True)
    rootfs = os.path.join(staging, "rootfs.hfs")
    ramdisk_len = os.path.getsize(rootfs) & ~0xFFF if os.path.exists(rootfs) else 0
    if ramdisk_len:
        import mkrootfs
        mkrootfs.build(staging, os.path.join(outdir, "rootfs.hfs"), verbose=verbose,
                       hfsplus=hfsplus, rootfs_add=rootfs_add)
        ramdisk_len = os.path.getsize(os.path.join(outdir, "rootfs.hfs")) & ~0xFFF
    shim = use_prebuilt_shim(outdir, prebuilt) if prebuilt else build_shim(outdir)
    # the DT size doesn't depend on the layout values, so two passes converge
    lay0 = layout(kva, len(kimg), len(template))
    lay0["ramdisk_len"] = ramdisk_len
    dt = dtgen.generate(template, lay0,
                        PHYS_BASE, VIRT_BASE, MEM_SIZE, len(kimg))
    lay = layout(kva, len(kimg), len(dt))
    lay["ramdisk_len"] = ramdisk_len
    dt = dtgen.generate(template, lay, PHYS_BASE, VIRT_BASE, MEM_SIZE, len(kimg))
    chk = layout(kva, len(kimg), len(dt))
    chk["ramdisk_len"] = ramdisk_len
    assert chk == lay
    applied = []
    if patches:
        syms = {"shim": lay["shim_pa"] - PHYS_BASE + VIRT_BASE}
        applied = patcher.apply_all(kimg, kva, os.path.join(ROOT, "kernel", "patches.json"), syms)

    entry_pa = mo.entry - VIRT_BASE + PHYS_BASE
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "kernel.bin"), "wb") as f:
        f.write(kimg)
    with open(os.path.join(outdir, "devtree.bin"), "wb") as f:
        f.write(dt)
    with open(os.path.join(outdir, "fakeio.bin"), "wb") as f:
        f.write(dtgen.fakeio_image())
    page = lambda n: dtgen.FAKEIO_BASE + dtgen.PAGE_MAP[n][0] * 0x1000 if n in dtgen.PAGE_MAP else 0
    configure_shim(outdir, {"vic0": page("vic"), "clcd": page("clcd"),
                            "dmac0": page("dmac0"), "i2s0": page("i2s0")})
    cfg = [
        "# ios3ds loader config - generated by scripts/mkboot.py",
        "# kernel sha1 %s, %d patches applied" % (hashlib.sha1(kimg).hexdigest(), len(applied)),
        "kernel=0:/ios3ds/kernel.bin",
        "devtree=0:/ios3ds/devtree.bin",
        "kernel_pa=0x%08x" % lay["kernel_pa"],
        "entry_pa=0x%08x" % entry_pa,
        "devtree_pa=0x%08x" % lay["devtree_pa"],
        "bootargs_pa=0x%08x" % lay["bootargs_pa"],
        "# fake S5L8720 register pages (AXI WRAM), see kernel/dtgen.py",
        "load=0:/ios3ds/fakeio.bin,0x%08x" % dtgen.FAKEIO_BASE,
        "# fake S5L CLCD registers: the ARM9 mirrors window 1 onto the top screen",
        "display_regs=0x%08x" % (dtgen.FAKEIO_BASE + dtgen.PAGE_MAP["clcd"][0] * 0x1000),
        "shim=0:/ios3ds/shim.bin",
        "# the shim's GPU part + the display mirror's PICA200 command lists (boot/common/gpu_mirror.h)",
        "load=0:/ios3ds/gpu.bin,0x%08x" % GPU_BIN_PA,
        ("rootfs=0:/ios3ds/rootfs.hfs" if ramdisk_len else "# no rootfs.hfs staged"),
        "shim_pa=0x%08x" % lay["shim_pa"],
        "ring_pa=0x%08x" % lay["ring_pa"],
        "ring_size=0x%x" % lay["ring_size"],
        "top_pa=0x%08x" % lay["top_pa"],
        "phys_base=0x%08x" % PHYS_BASE,
        "virt_base=0x%08x" % VIRT_BASE,
        "mem_size=0x%08x" % MEM_SIZE,
        "# boot_args header: 4.2.1's pe_identify_machine panics 'Epoch Mismatch' unless version=5",
        "ba_revision=1",
        "ba_version=5",
        "# Video: framebuffer handed to XNU (0 = none)",
        "video_pa=0x0",
        "video_display=0",
        "video_rowbytes=0",
        "video_width=0",
        "video_height=0",
        "video_depth=0",
        "# ARM11 instruction probes (set 0 if the ARM11 hangs in one)",
        "probes=1",
        "# set jump=0 to stop before starting XNU",
        "jump=1",
        "countdown_ms=2000",
        "# iOS screen mode, X cycles at run time: 0 = top (log on bottom), 1 = both (bottom = touch map), 2 = bottom only",
        "screen=1",
        "# PICA200 GPU (docs/pica200.md): gpu=0 keeps the kernel away from the GPU entirely",
        "# (GL falls back to the CPU rasterizer, the ARM9 copies the screen); gpu_mirror=0",
        "# keeps only the screen copy on the ARM9. The mirror self-tests itself and falls back",
        "# to the ARM9 copy on any mismatch or timeout. gpu_vbl_div: a GPU frame at most every",
        "# Nth VBlank (2 = 30 fps). gpu_spin_us: IRQ-time wait for the GPU (0 = never wait)",
        "gpu=1",
        "gpu_mirror=1",
        "gpu_vbl_div=2",
        "gpu_spin_us=0",
        "# gpu_clk=1: set the GX clock register to 0x70100 (GSP/libn3ds value) if Luma left it 0;",
        "# gpu_diag=1: staged GPU tests at bring-up, logged as [gpu-diag] (docs/pica200.md section 12)",
        "gpu_clk=1",
        "gpu_diag=1",
        "# system controller (docs/mcu.md): RTC, battery, HOME/POWER, sliders, accelerometer.",
        "# mcu=0 never touches it (clock at 1970, no battery). rtc_offset: seconds added to the",
        "# 3DS RTC to get UTC, e.g. -3600 if the RTC runs on CET (UTC+1)",
        "mcu=1",
        "rtc_offset=0",
        "# XNU boot-args (edit freely, max 255 chars)",
        "cmdline=%s" % (cmdline if cmdline is not None else DEFAULT_CMDLINE),
        "",
    ]
    with open(os.path.join(outdir, "boot.cfg"), "w", newline="\r\n") as f:
        f.write("\n".join(cfg))
    if verbose:
        print("[mkboot] kernel %d bytes @0x%08x, entry 0x%08x, %d patches" % (
            len(kimg), lay["kernel_pa"], entry_pa, len(applied)))
        for p in applied:
            print("[mkboot]   patch %s @0x%08x: %s" % (p["name"], p["va"], p["why"]))
        print("[mkboot] devtree %d bytes @0x%08x, boot_args @0x%08x, shim %d bytes @0x%08x, ring @0x%08x, top 0x%08x" % (
            len(dt), lay["devtree_pa"], lay["bootargs_pa"], len(shim), lay["shim_pa"], lay["ring_pa"], lay["top_pa"]))
    return lay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd", "ios3ds"))
    ap.add_argument("--cmdline", default=None)
    ap.add_argument("--no-patches", action="store_true")
    ap.add_argument("--shim-only", metavar="DIR",
                    help="only compile shim.bin + gpu.bin into DIR (unconfigured; for releases)")
    a = ap.parse_args()
    if a.shim_only:
        os.makedirs(a.shim_only, exist_ok=True)
        build_shim(a.shim_only)
        print("[mkboot] shim.bin + gpu.bin -> %s" % a.shim_only)
        return
    build(a.staging, a.out, a.cmdline, patches=not a.no_patches)


if __name__ == "__main__":
    main()
