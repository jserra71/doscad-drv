#!/bin/sh
# SPDX-FileCopyrightText: 2026 Joaquim Serra
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Developed by Claude Opus 5.5 in high effort mode.
#
# Builds DOSCAD.DRV.
#  1. Checks that every tool the build needs is installed.
#  2. Assembles the stub (nasm) and compiles the C code (gcc -m16).
#  3. Links the flat image at offset 0 (the driver itself).
#  4. Compresses it (lzpack.py) and assembles the loader stub around it.
#  5. Prepends a 32-byte MZ header to the stub.
# The 8x8 font is OrCAD's and is not part of the build: the driver reads it from
# the user's VGA640.DRV when it starts (doscad.c, load_font).
#
# Usage: sh build.sh
# Environment overrides: DOSCAD_OUT (build directory), DOSCAD_DRV (the finished
# driver), DOSCAD_CFLAGS (extra gcc flags).
set -eu

# 1. Every missing tool is named at once, so one install fixes them all.
missing=""
for tool in nasm gcc ld objcopy nm python3 stat awk sed grep dirname mkdir mktemp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing="$missing $tool"
    fi
done
if [ -n "$missing" ]; then
    echo "build.sh: missing tools:$missing" >&2
    echo "Install them from your distribution (nasm, gcc, binutils, python3, coreutils, awk, sed)." >&2
    exit 1
fi
# gcc must also make 16-bit i386 code (-m16); a gcc without x86 32-bit support cannot.
probe=$(mktemp)
if ! echo 'int f(void) { return 0; }' | gcc -m16 -march=i386 -ffreestanding -x c -c -o "$probe" - 2>/dev/null; then
    rm -f "$probe"
    echo "build.sh: gcc cannot compile with -m16 -march=i386 (needs an x86 gcc with 32-bit support)." >&2
    exit 1
fi
rm -f "$probe"
if ! ld -V 2>/dev/null | grep -q elf_i386; then
    echo "build.sh: ld has no elf_i386 emulation (needs GNU ld for x86)." >&2
    exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
# Inputs and outputs, each overridable; the defaults are next to the script.
out=${DOSCAD_OUT:-"$here/build"}
drv=${DOSCAD_DRV:-"$here/DOSCAD.DRV"}
mkdir -p "$out"

# 2. -O2 without function inlining and alignment padding: about 11 % faster
# redraws than -Os and still 3 KB under the 32 KB block. The trace
# build (DOSCAD_CFLAGS=-DDOSCAD_TRACE) carries a large buffer, so it stays at
# -Os to fit the 64 KB image.
opt="-O2 -fno-inline-functions -fno-align-functions -fno-align-jumps -fno-align-loops -fno-align-labels"
case "${DOSCAD_CFLAGS:-}" in
*DOSCAD_TRACE*) opt="-Os" ;;
esac

nasm -f elf32 -o "$out/doscad-asm.o" "$here/doscad.asm"
gcc -m16 -march=i386 $opt -ffreestanding -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
    -fno-stack-protector -fno-builtin -Wall -Wextra ${DOSCAD_CFLAGS:-} \
    -c -o "$out/doscad-c.o" "$here/doscad.c"
# 3. The flat image.
(cd "$out" && ld -m elf_i386 -T "$here/link.ld" -o image.elf doscad-asm.o doscad-c.o)
objcopy -O binary "$out/image.elf" "$out/image.bin"
table=$(nm "$out/image.elf" | awk '$3 == "entry_table" {print "0x" $1}')
old16=$(nm "$out/image.elf" | awk '$3 == "old16" {print "0x" $1}')
kbhooked=$(nm "$out/image.elf" | awk '$3 == "keyboard_hooked" {print "0x" $1}')
# The build's identity, which the stub looks for in a resident image: the first
# 12 bytes of the image's SHA-256, as a nasm byte list.
image_id=$(python3 -I -c 'import hashlib, sys; print(",".join("0x%02x" % b for b in hashlib.sha256(open(sys.argv[1], "rb").read()).digest()[:12]))' "$out/image.bin")

# 4. PCB_RM.EXE loads the driver into a fixed 32 KB block: the stub carries the
# image compressed and unpacks it into a block of its own (stub.asm).
python3 -I "$here/lzpack.py" "$out/image.bin" "$out/image.lz"
size=$(stat -c %s "$out/image.bin")
nasm -f bin -DFULL_PARAS=$(( (size + 15) / 16 + 2 )) -DTABLE="$table" \
    -DOLD16="$old16" -DKBHOOKED="$kbhooked" -DIMAGE_ID="$image_id" \
    -DPAYLOAD="'$out/image.lz'" -o "$out/stub.bin" "$here/stub.asm"

# The version lives in one place, DOSCAD_VERSION in doscad.c.
version=$(sed -n 's/^#define DOSCAD_VERSION "\(.*\)"$/\1/p' "$here/doscad.c")

# 5. The MZ header.
python3 -I - "$out/stub.bin" "$drv" "$size" "$version" <<'EOF'
import struct, sys
stub = open(sys.argv[1], "rb").read()
total = len(stub) + 32
if total > 0x8000:
    sys.exit("DOSCAD.DRV: %d bytes, over the 32 KB block of PCB_RM.EXE" % total)
pages = (total + 511) // 512
last = total % 512
header = struct.pack("<2s13H", b"MZ", last, pages, 0, 2, 0, 0xffff, 0, 0, 0, 0, 0, 0x1e, 0)
header += b"\0" * (32 - len(header))
open(sys.argv[2], "wb").write(header + stub)
print("DOSCAD.DRV v%s: %d bytes; image %s bytes -> %s" % (sys.argv[4], total, sys.argv[3], sys.argv[2]))
EOF
