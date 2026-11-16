#!/usr/bin/env bash
# Build xpwn's 'hfsplus' HFS+ image editor (GPL-3.0, planetbeing/xpwn) into build/tools/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/xpwn"
[[ -d "$SRC/.git" ]] || git clone -q https://github.com/planetbeing/xpwn.git "$SRC"
git -C "$SRC" checkout -q 20c32e5c12d1b22a9d55a59a0ff6267f539b77f4
mkdir -p "$ROOT/build/tools"
(cd "$SRC" && gcc -O2 -w -Iincludes hfs/*.c common/abstractfile.c common/base64.c -lz -o "$ROOT/build/tools/hfsplus")
echo "built $ROOT/build/tools/hfsplus"
