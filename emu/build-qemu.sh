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
grep -q nintendo3ds "$Q/hw/arm/meson.build" || \
    sed -i "2a arm_common_ss.add(when: 'CONFIG_NINTENDO3DS', if_true: files('nintendo3ds.c'))" "$Q/hw/arm/meson.build"
grep -q NINTENDO3DS "$Q/hw/arm/Kconfig" || cat >> "$Q/hw/arm/Kconfig" <<'KC'

config NINTENDO3DS
    bool
    default y
    depends on TCG && ARM
    select ARM11MPCORE
    select SD
    select UNIMP
KC
mkdir -p "$Q/b"
if [[ ! -f "$Q/b/build.ninja" ]]; then
    (cd "$Q/b" && ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
        --disable-kvm --disable-xen --disable-vnc --disable-sdl --disable-gtk --disable-opengl \
        --disable-curl --disable-libssh --disable-capstone --disable-slirp --disable-brlapi \
        --disable-tools --disable-guest-agent)
fi
ninja -C "$Q/b" qemu-system-arm
echo "built: $Q/b/qemu-system-arm"
