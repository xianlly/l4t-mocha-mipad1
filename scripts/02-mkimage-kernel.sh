#!/bin/bash
# 02-mkimage-kernel.sh
# Wrap the ORIGINAL L4T zImage into a uImage that U-Boot's `bootm` accepts.
#
#   -C none : the zImage is self-decompressing - do NOT let mkimage compress it
#   -a/-e   : Tegra124 loads the kernel at 0x80008000
#
# Usage: ./02-mkimage-kernel.sh <orig-zImage> <out-uImage>
set -eu

usage() {
  cat <<'EOF'
Usage: 02-mkimage-kernel.sh <orig-zImage> <out-uImage>

Wrap the original L4T zImage in a uImage header so U-Boot's `bootm` can load it.

  mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000

Arguments:
  <orig>   original zImage from L4T_R241.zip:root_dir/boot/zImage (7,818,464 bytes)
  <out>    output file to install as /l4t/boot/uImage-desktop

Options:
  -h, --help   show this help

Requires: u-boot-tools (mkimage)

NOTE: this wraps the STOCK kernel. Do not substitute a self-built kernel - see
      README.md section 5.2 / the "What NOT to do" list.
EOF
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
[ $# -eq 2 ] || { echo "ERROR: expected 2 arguments, got $#" >&2; usage >&2; exit 2; }
IN="$1"; OUT="$2"
[ -f "$IN" ] || { echo "ERROR: input not found: $IN" >&2; exit 2; }
command -v mkimage >/dev/null || { echo "ERROR: missing mkimage (apt install u-boot-tools)" >&2; exit 2; }

size=$(stat -c%s "$IN" 2>/dev/null || echo 0)
echo "==> input: $IN ($size bytes)"
if [ "$size" -lt 1000000 ]; then
  echo "WARNING: input is suspiciously small for a kernel image" >&2
fi

mkimage -A arm -O linux -T kernel -C none \
        -a 0x80008000 -e 0x80008000 \
        -n "L4T-ORIGINAL" \
        -d "$IN" "$OUT"

echo "==> done"
ls -l "$OUT"
sha256sum "$OUT"
