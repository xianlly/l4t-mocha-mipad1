#!/bin/bash
# 06-install-to-data.sh
# Install the L4T files onto p29 (UDA) and set up the Wi-Fi firmware symlinks.
# Run this from the project's RAM installer, or any Linux with p29 not mounted rw.
#
# Usage: ./06-install-to-data.sh <dir with uImage/initramfs/dtb/rootfs>
set -eu

usage() {
  cat <<'EOF'
Usage: 06-install-to-data.sh <dir> [partition] [mountpoint]

Install L4T onto the data partition:

  /l4t/boot/uImage-desktop            <- <dir>/uImage-desktop
  /l4t/boot/initramfs.cpio.gz         <- <dir>/initramfs-mocha-data.cpio.gz
  /l4t/boot/mocha-desktop.dtb         <- <dir>/dtb-final.dtb
  /l4t/root/...                       <- <dir>/root_00.tar.gz
  /l4t/root/etc/firmware/*            <- Wi-Fi firmware symlinks

Arguments:
  <dir>           directory holding the four files above
  [partition]     target partition, default /dev/mmcblk0p29
  [mountpoint]    mount point, default /mnt

Options:
  -h, --help   show this help

Environment:
  PART   same as [partition]
  MNT    same as [mountpoint]

Requires: root, an unmounted (or read-only) p29, tar, gzip
EOF
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
[ $# -ge 1 ] && [ $# -le 3 ] || { echo "ERROR: expected 1-3 arguments, got $#" >&2; usage >&2; exit 2; }
D="$1"
PART="${2:-${PART:-/dev/mmcblk0p29}}"
MNT="${3:-${MNT:-/mnt}}"

[ -d "$D" ] || { echo "ERROR: not a directory: $D" >&2; exit 2; }
[ "$(id -u)" = "0" ] || { echo "ERROR: must run as root (or via sudo)" >&2; exit 2; }
[ -b "$PART" ] || { echo "ERROR: $PART is not a block device" >&2; exit 2; }

missing=0
for f in uImage-desktop initramfs-mocha-data.cpio.gz dtb-final.dtb root_00.tar.gz; do
  if [ ! -f "$D/$f" ]; then echo "ERROR: missing $D/$f" >&2; missing=1; fi
done
[ "$missing" = "0" ] || { echo "See README.md section 5 and scripts/01..05 to generate these." >&2; exit 2; }

if mount | grep -qE "^$PART on $MNT "; then
  echo "WARNING: $PART already mounted on $MNT - reusing it"
else
  echo "==> making eMMC writable"
  blockdev --setrw "$(echo "$PART" | sed 's/p[0-9]*$//')" 2>/dev/null || true
  echo "==> mounting $PART on $MNT"
  mkdir -p "$MNT"
  mount "$PART" "$MNT" || { echo "ERROR: mount failed (is p29 already mounted elsewhere?)" >&2; exit 1; }
fi

mkdir -p "$MNT/l4t/boot" "$MNT/l4t/root"

echo "==> installing boot files"
cp -f "$D/uImage-desktop"                "$MNT/l4t/boot/uImage-desktop"
cp -f "$D/initramfs-mocha-data.cpio.gz"  "$MNT/l4t/boot/initramfs.cpio.gz"
cp -f "$D/dtb-final.dtb"                 "$MNT/l4t/boot/mocha-desktop.dtb"

echo "==> extracting rootfs"
if [ -z "$(ls -A "$MNT/l4t/root" 2>/dev/null)" ]; then
  tar -xzf "$D/root_00.tar.gz" -C "$MNT/l4t/root"
else
  echo "    /l4t/root is not empty - skipping extraction"
  echo "    (wipe it first if you really want a clean install)"
fi

echo "==> Wi-Fi firmware symlinks"
mkdir -p "$MNT/l4t/root/etc/firmware"
ln -sf /lib/firmware/fw_bcmdhd.bin "$MNT/l4t/root/etc/firmware/fw_bcmdhd.bin"
ln -sf /lib/firmware/nvram.txt     "$MNT/l4t/root/etc/firmware/nvram.txt"

echo "==> installed:"
ls -l "$MNT/l4t/boot"
echo "==> sync + unmount"
sync
umount "$MNT" || true
blockdev --setro "$(echo "$PART" | sed 's/p[0-9]*$//')" 2>/dev/null || true
echo "DONE - reboot the device"
