#!/usr/bin/env bash
# ios3ds toolchain check / setup (Linux).
#
# 3DS side : devkitARM (preferred, https://devkitpro.org/wiki/Getting_Started)
#            or any arm-none-eabi-gcc >= 12 (e.g. the Arm GNU Toolchain).
# Kernel side (M0-M2): python3 (+ 'cryptography' or the openssl CLI) for
#            scripts/fwprep.py; llvm-objdump for analysis.
#            M2+ will add clang + an ld64 port (cctools-port) to build Mach-O
#            code for the kernelcache; not needed yet.
#
#   scripts/setup-toolchain.sh            # report what's found
#   scripts/setup-toolchain.sh --install-arm-gnu [DIR]
#            # no-root fallback: download the Arm GNU Toolchain to DIR (~1 GB)
set -euo pipefail

ARM_GNU_VER=14.2.rel1
ARM_GNU_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GNU_VER}/binrel/arm-gnu-toolchain-${ARM_GNU_VER}-x86_64-arm-none-eabi.tar.xz"

if [[ "${1:-}" == "--install-arm-gnu" ]]; then
    dest="${2:-$HOME/opt}"
    mkdir -p "$dest"
    echo "downloading $ARM_GNU_URL -> $dest"
    curl -fL "$ARM_GNU_URL" | tar -xJ -C "$dest"
    echo "add to PATH: $dest/arm-gnu-toolchain-${ARM_GNU_VER}-x86_64-arm-none-eabi/bin"
    exit 0
fi

ok=1
find_cc() {
    if [[ -n "${DEVKITARM:-}" && -x "$DEVKITARM/bin/arm-none-eabi-gcc" ]]; then
        echo "$DEVKITARM/bin/arm-none-eabi-"; return
    fi
    if [[ -x /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc ]]; then
        echo "/opt/devkitpro/devkitARM/bin/arm-none-eabi-"; return
    fi
    if command -v arm-none-eabi-gcc >/dev/null; then
        echo "$(dirname "$(command -v arm-none-eabi-gcc)")/arm-none-eabi-"; return
    fi
    for d in "$HOME"/opt/arm-gnu-toolchain-*-arm-none-eabi/bin; do
        [[ -x "$d/arm-none-eabi-gcc" ]] && { echo "$d/arm-none-eabi-"; return; }
    done
}
CROSS=$(find_cc || true)
if [[ -n "$CROSS" ]]; then
    echo "[ok]   cross compiler: ${CROSS}gcc ($("${CROSS}gcc" -dumpversion))"
    echo "       (Makefile picks this up automatically; override with CROSS=...)"
else
    ok=0
    echo "[miss] no arm-none-eabi-gcc. Install devkitARM:"
    echo "         https://devkitpro.org/wiki/devkitPro_pacman  then  sudo dkp-pacman -S devkitARM"
    echo "       or run: $0 --install-arm-gnu"
fi

if command -v python3 >/dev/null; then
    echo "[ok]   python3 $(python3 -c 'import sys;print(sys.version.split()[0])')"
    if python3 -c 'import cryptography' 2>/dev/null; then
        echo "[ok]   python cryptography"
    elif command -v openssl >/dev/null; then
        echo "[ok]   openssl CLI (fallback AES backend; slower)"
    else
        ok=0; echo "[miss] need python3-cryptography or openssl"
    fi
else
    ok=0; echo "[miss] python3"
fi

for t in llvm-objdump llvm-objdump-18 llvm-objdump-19 arm-none-eabi-objdump; do
    if command -v "$t" >/dev/null; then echo "[ok]   disassembler: $t"; break; fi
done

[[ $ok == 1 ]] && echo "toolchain OK" || { echo "toolchain incomplete"; exit 1; }
