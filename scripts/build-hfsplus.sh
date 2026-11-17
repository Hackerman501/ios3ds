#!/usr/bin/env bash
# Build xpwn's 'hfsplus' HFS+ image editor (GPL-3.0, planetbeing/xpwn) into build/tools/.
#   scripts/build-hfsplus.sh            build/tools/hfsplus (needs a C compiler + zlib headers)
#   scripts/build-hfsplus.sh --static   also build/tools/hfsplus-linux-<arch> (static, for releases)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/xpwn"
REV=20c32e5c12d1b22a9d55a59a0ff6267f539b77f4
[[ -d "$SRC/.git" ]] || git clone -q https://github.com/planetbeing/xpwn.git "$SRC"
git -C "$SRC" checkout -q "$REV"
mkdir -p "$ROOT/build/tools"
# scripts/install.py compiles the same files from a release zip's src/xpwn-hfsplus/
FILES='hfs/*.c common/abstractfile.c common/base64.c'
# shellcheck disable=SC2086  # FILES is globbed inside $SRC
(cd "$SRC" && ${CC:-gcc} -O2 -w -Iincludes $FILES -lz -o "$ROOT/build/tools/hfsplus")
echo "built $ROOT/build/tools/hfsplus"
if [[ "${1:-}" == "--static" ]]; then
    out="$ROOT/build/tools/hfsplus-linux-$(uname -m)"
    # shellcheck disable=SC2086
    (cd "$SRC" && ${CC:-gcc} -O2 -w -Iincludes $FILES -static -s -lz -o "$out")
    echo "built $out"
fi
