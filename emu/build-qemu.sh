#!/usr/bin/env bash
# Build QEMU with the ios3ds "nintendo3ds" machine into build/qemu.
#   emu/build-qemu.sh          (first run clones QEMU v10.2.4, ~10 min)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="$ROOT/build/qemu"
VER=v10.2.4
if [[ ! -d "$Q/.git" ]]; then
    git clone -q --depth 1 --branch "$VER" https://gitlab.com/qemu-project/qemu.git "$Q"
fi
# QEMU core patches (idempotent)
for p in "$ROOT"/emu/qemu/patches/*.patch; do
    git -C "$Q" apply --check "$p" 2>/dev/null && git -C "$Q" apply "$p" && echo "applied $(basename "$p")"
done
# our machine: symlink so edits in emu/ are picked up by ninja
ln -sf "$ROOT/emu/qemu/hw/arm/nintendo3ds.c" "$Q/hw/arm/nintendo3ds.c"
# the software PICA200 model behind the machine's GX block (docs/pica200.md)
ln -sf "$ROOT/tools/ios3ds-pica/sim/picasim.c" "$Q/hw/arm/picasim.c"
ln -sf "$ROOT/tools/ios3ds-pica/sim/picasim.h" "$Q/hw/arm/picasim.h"
# the CSND sound block model (docs/audio.md section 6)
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_csnd.c" "$Q/hw/arm/ctr_csnd.c"
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_csnd.h" "$Q/hw/arm/ctr_csnd.h"
# the camera model (docs/camera.md section 5) and the headers it shares with ios3ds
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_cam.c" "$Q/hw/arm/ctr_cam.c"
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_cam.h" "$Q/hw/arm/ctr_cam.h"
ln -sf "$ROOT/boot/common/cam_proto.h" "$Q/hw/arm/cam_proto.h"
ln -sf "$ROOT/boot/common/cam_pattern.h" "$Q/hw/arm/cam_pattern.h"
# the Wi-Fi model: TMIO SDIO controller 2 + AR6014 + WPA2 access point (docs/wifi.md section 7)
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_wifi.c" "$Q/hw/arm/ctr_wifi.c"
ln -sf "$ROOT/emu/qemu/hw/arm/ctr_wifi.h" "$Q/hw/arm/ctr_wifi.h"
grep -q nintendo3ds "$Q/hw/arm/meson.build" || \
    sed -i "2a arm_common_ss.add(when: 'CONFIG_NINTENDO3DS', if_true: files('nintendo3ds.c'))" "$Q/hw/arm/meson.build"
grep -q picasim "$Q/hw/arm/meson.build" || \
    sed -i "s/files('nintendo3ds.c')/files('nintendo3ds.c', 'picasim.c')/" "$Q/hw/arm/meson.build"
grep -q ctr_csnd "$Q/hw/arm/meson.build" || \
    sed -i "s/'picasim.c')/'picasim.c', 'ctr_csnd.c')/" "$Q/hw/arm/meson.build"
grep -q ctr_wifi "$Q/hw/arm/meson.build" || \
    sed -i "s/'ctr_csnd.c')/'ctr_csnd.c', 'ctr_wifi.c')/" "$Q/hw/arm/meson.build"
grep -q ctr_cam "$Q/hw/arm/meson.build" || \
    sed -i "s/'ctr_wifi.c')/'ctr_wifi.c', 'ctr_cam.c')/" "$Q/hw/arm/meson.build"
grep -q NINTENDO3DS "$Q/hw/arm/Kconfig" || cat >> "$Q/hw/arm/Kconfig" <<'KC'

config NINTENDO3DS
    bool
    default y
    depends on TCG && ARM
    select ARM11MPCORE
    select SD
    select UNIMP
KC
# user-mode networking (slirp; meson fetches libslirp as a subproject when
# the host has none) for the Wi-Fi model's access point: a build configured
# without it is redone once
if [[ -f "$Q/b/build.ninja" ]] && ! grep -q "#define CONFIG_SLIRP" "$Q/b/config-host.h" 2>/dev/null; then
    rm -rf "$Q/b"
fi
mkdir -p "$Q/b"
if [[ ! -f "$Q/b/build.ninja" ]]; then
    (cd "$Q/b" && ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
        --disable-kvm --disable-xen --disable-vnc --disable-sdl --disable-gtk --disable-opengl \
        --disable-curl --disable-libssh --disable-capstone --enable-slirp --disable-brlapi \
        --disable-tools --disable-guest-agent)
fi
ninja -C "$Q/b" qemu-system-arm
echo "built: $Q/b/qemu-system-arm"
