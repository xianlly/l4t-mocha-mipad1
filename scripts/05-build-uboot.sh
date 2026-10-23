#!/bin/bash
# 05-build-uboot.sh
# Patch the mocha-moze U-Boot environment to boot L4T from the data partition (p29) and
# build the boot image for p22 (LNX).
#
# Usage: ./05-build-uboot.sh <mocha-moze-boot checkout> <work dir> [jobs]
set -eu

usage() {
  cat <<'EOF'
Usage: 05-build-uboot.sh <mocha-moze-boot> <workdir> [jobs]

Patch overlay/board/xiaomi/mocha/mocha_emmc_boot.env so U-Boot loads L4T from p29:

  ext4load mmc 0#UDA <addr> /l4t/boot/uImage-desktop
  ext4load mmc 0#UDA <addr> /l4t/boot/initramfs.cpio.gz
  ext4load mmc 0#UDA <addr> /l4t/boot/mocha-desktop.dtb
  setenv bootargs <full mocha cmdline, no mem=, root=/dev/mmcblk0p29 rootsubdir=/l4t/root>

then builds u-boot and packs mocha-uboot.img for p22.

Arguments:
  <mocha-moze-boot>   git checkout of the mocha-moze-boot project
  <workdir>           build output directory
  [jobs]              parallel make jobs (default 8)

Options:
  -h, --help   show this help

Environment:
  TC   optional cross-compiler prefix (e.g. arm-linux-gnueabihf-);
       otherwise tools/build.sh picks what it finds in PATH

Requires: a Linaro/arm 32-bit cross toolchain, python3

IMPORTANT - U-Boot parses `mmc dev:part` partition numbers as HEX:
  `mmc 0:29` becomes partition 0x29 = 41 -> "** Invalid partition 41 **"
This script uses the GPT partition NAME (`mmc 0#UDA`) which is not affected.
EOF
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
[ $# -ge 2 ] && [ $# -le 3 ] || { echo "ERROR: expected 2 or 3 arguments, got $#" >&2; usage >&2; exit 2; }
SRC="$1"; OUT="$2"; JOBS="${3:-8}"

[ -d "$SRC" ] || { echo "ERROR: source dir not found: $SRC" >&2; exit 2; }
ENV="$SRC/overlay/board/xiaomi/mocha/mocha_emmc_boot.env"
[ -f "$ENV" ] || { echo "ERROR: $ENV not found - not a mocha-moze-boot checkout?" >&2; exit 2; }
[ -f "$SRC/tools/build.sh" ] || { echo "ERROR: $SRC/tools/build.sh not found" >&2; exit 2; }
[ -f "$SRC/tools/pack-android.py" ] || { echo "ERROR: $SRC/tools/pack-android.py not found" >&2; exit 2; }
case "$JOBS" in ''|*[!0-9]*) echo "ERROR: jobs must be a number" >&2; exit 2 ;; esac
command -v python3 >/dev/null || { echo "ERROR: missing python3" >&2; exit 2; }

BOOTARGS='tegraid=40.1.1.0.0 otf_key=c765c04dccd12501f0ba2154786d2c92 panel_id=32 memtype=0 tsec=32M@4064M tzram=4M@4058M ddr_die=1024M@2048M section=128M fbcon=rotate:1 commchip_id=0 emc_max_dvfs=0 touch_id=0@2150632068 video=tegrafb no_console_suspend=1 console=ttyS0,115200n8 console=tty0 debug_uartport=lsport,3 sku_override=0 usbcore.old_scheme_first=1 lp0_vec=2896@0xfdfff000 tegra_fbmem=25296896@0xad012000 nvdumper_reserved=0xfd700000 core_edp_mv=1150 core_edp_ma=4000 pmuboard=0x06c8:0x03e8:0x00:0x44:0x04 displayboard=0x8038:0x0000:0x00:0x00:0x00 power_supply=Battery board_info=0x06f4:0x044c:0x03:0x41:0x07 tegraboot=sdmmc gpt gpt_sector=41983 modem_id=1 watchdog=disable nck=1048576@0xfd700000 rw root=/dev/mmcblk0p29 rootsubdir=/l4t/root rootfstype=ext4 rootflags=defaults,noatime,nodiratime'

NEW="boot_debian=setenv fdt_high ffffffff; setenv initrd_high ffffffff; if mmc dev 0 && ext4load mmc 0#UDA 80007fc0 /l4t/boot/uImage-desktop && ext4load mmc 0#UDA 88000000 /l4t/boot/initramfs.cpio.gz; then setenv rdsize \${filesize}; if ext4load mmc 0#UDA 8c000000 /l4t/boot/mocha-desktop.dtb; then fdt addr 8c000000; fdt resize 10000; setenv bootargs $BOOTARGS; bootm 80007fc0 88000000:\${rdsize} 8c000000; fi; fi"

echo "==> patching $ENV"
cp -n "$ENV" "$ENV.bak" 2>/dev/null || true
python3 - "$ENV" "$NEW" <<'PY'
import sys
p, new = sys.argv[1], sys.argv[2]
lines = open(p).read().split('\n')
out, done = [], False
for ln in lines:
    if ln.startswith('boot_debian='):
        out.append(new); done = True
    else:
        out.append(ln)
if not done:
    out.append(new)
open(p, 'w').write('\n'.join(out))
print("boot_debian line replaced" if done else "boot_debian line appended")
PY

echo "==> building (jobs=$JOBS)"
mkdir -p "$OUT"
( cd "$SRC" && JOBS="$JOBS" bash tools/build.sh "$OUT" )
python3 "$SRC/tools/pack-android.py" "$OUT/u-boot-dtb.bin" "$OUT/u-boot" "$OUT/mocha-uboot.img"

echo "==> verify the bootargs made it into the image"
n_args=$(grep -c 'tegraid=40.1.1.0.0' "$OUT/u-boot-dtb.bin" || true)
n_uda=$(grep -c 'mmc 0#UDA' "$OUT/u-boot-dtb.bin" || true)
echo "  bootargs occurrences: $n_args (expect >=1)"
echo "  mmc 0#UDA occurrences: $n_uda (expect >=3)"
[ "$n_args" -ge 1 ] || { echo "ERROR: bootargs missing from u-boot-dtb.bin" >&2; exit 1; }
[ "$n_uda" -ge 3 ] || { echo "ERROR: UDA load paths missing from u-boot-dtb.bin" >&2; exit 1; }

echo "==> done"
ls -l "$OUT/mocha-uboot.img"
sha256sum "$OUT/mocha-uboot.img"
