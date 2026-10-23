#!/bin/bash
# 04-build-dtb-nopmoff.sh
# Remove the palmas power-off binding from the DTB so the power button keeps working
# after a shutdown.
#
# BACKGROUND
#   drivers/power/reset/palmas-poweroff.c registers pm_power_off when the DT node has the
#   "system-pmic-power-off" property. Its palmas_power_off():
#       palmas_write(..., SWOFF_COLDRST, 0x0);        <- masks every cold-reset source,
#                                                        including PWRON_LPK (power-key
#                                                        long press)
#       palmas_update_bits(..., DEV_CTRL, 1, 0);      <- soft power-off
#   On this PMIC (tps65913, SWOFF_COLDRST reads 0x5b and rejects writes) that leaves the
#   device in a state only a LONG press can wake - and the long-press bit is masked.
#   Result: the unit appears bricked until the battery is disconnected.
#
# FIX
#   Delete "system-pmic-power-off;" from /i2c@7000d000/tps65913/power_pm.
#   Without it the kernel has no pm_power_off and halts instead. The PMIC keeps PWRDOWN
#   enabled, so pressing the power button performs a cold reset -> the device boots.
#
# Usage: ./04-build-dtb-nopmoff.sh <in.dtb> <out.dtb>
set -eu

usage() {
  cat <<'EOF'
Usage: 04-build-dtb-nopmoff.sh <in.dtb> <out.dtb>

Make the power button work after a shutdown by deleting the palmas power-off binding:

  /i2c@7000d000/tps65913/power_pm/system-pmic-power-off

The kernel then has no pm_power_off and halts instead of entering the PMIC soft-off
state that the power button cannot wake from. "system-pmic-power-reset" is kept, so
reboot still works.

Arguments:
  <in>     DTB, usually the output of 03-build-dtb-3gb.sh
           (or the original tegra124-mocha.dtb on a 2 GB unit)
  <out>    final DTB to install as /l4t/boot/mocha-desktop.dtb

Options:
  -h, --help   show this help

Requires: device-tree-compiler (dtc), python3

Verification after flashing - on the running device:
  test -e /proc/device-tree/i2c@7000d000/tps65913/power_pm/system-pmic-power-off \
    && echo "fix NOT applied" || echo "no pm_power_off (good)"
EOF
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
[ $# -eq 2 ] || { echo "ERROR: expected 2 arguments, got $#" >&2; usage >&2; exit 2; }
IN="$1"; OUT="$2"
[ -f "$IN" ] || { echo "ERROR: input not found: $IN" >&2; exit 2; }
for tool in dtc python3; do
  command -v "$tool" >/dev/null || { echo "ERROR: missing required tool: $tool" >&2; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

dtc -I dtb -O dts -o "$WORK/in.dts" "$IN" 2>/dev/null || {
  echo "ERROR: cannot decompile $IN - is it a valid DTB?" >&2; exit 1; }
BEFORE=$(grep -c '{' "$WORK/in.dts")

python3 - "$WORK/in.dts" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
n = s.count('system-pmic-power-off')
if n != 1:
    print("ERROR: expected exactly 1 'system-pmic-power-off', found %d" % n, file=sys.stderr)
    raise SystemExit(1)
s = re.sub(r'\n\s*system-pmic-power-off;', '', s, count=1)
open(p, 'w').write(s)
print("removed system-pmic-power-off (system-pmic-power-reset kept)")
PY

dtc -I dts -O dtb -o "$OUT" "$WORK/in.dts" 2>/dev/null || {
  echo "ERROR: recompiling the DTB failed" >&2; exit 1; }
AFTER=$(dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -c '{')
echo "==> nodes before/after: $BEFORE/$AFTER"
[ "$BEFORE" = "$AFTER" ] || { echo "ERROR: node count changed" >&2; exit 1; }

echo "==> verify (expect power-off: 0, power-reset: 1):"
off=$(dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -c 'system-pmic-power-off' || true)
rst=$(dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -c 'system-pmic-power-reset' || true)
echo "  power-off  present: $off"
echo "  power-reset present: $rst"
[ "$off" = "0" ] || { echo "ERROR: property still present" >&2; exit 1; }

echo "==> done"
ls -l "$OUT"
sha256sum "$OUT"
