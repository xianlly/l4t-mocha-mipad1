#!/bin/bash
# 01-patch-initramfs.sh
# Patch the ORIGINAL L4T initramfs so it understands mocha (Mi Pad 1) partition numbers.
#
# Why: the stock initramfs hardcodes Shield Tablet partition numbers and adds an LTE
# offset when /dev/mmcblk0p25 exists (mocha has it), so every mount fails.
#
# Usage: ./01-patch-initramfs.sh <orig-initramfs.cpio.gz> <out-initramfs.cpio.gz>
set -eu

usage() {
  cat <<'EOF'
Usage: 01-patch-initramfs.sh <orig-initramfs.cpio.gz> <out-initramfs.cpio.gz>

Repack the original L4T initramfs with mocha (Mi Pad 1) partition numbers:
  DATAPART=p29  SYSPART=p26  FACTORYPART=p18  CALIBPART=p8  CACHEPART=p28
  PARTOFF=0   (the LTE offset must be disabled - mocha has /dev/mmcblk0p25)
  the four extra mounts are also made non-fatal

Arguments:
  <orig>   original initramfs from L4T_R241.zip:root_dir/boot/initramfs.cpio.gz
  <out>    output file to install as /l4t/boot/initramfs.cpio.gz

Options:
  -h, --help   show this help

Requires: cpio, gzip, python3
EOF
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
[ $# -eq 2 ] || { echo "ERROR: expected 2 arguments, got $#" >&2; usage >&2; exit 2; }
IN="$1"; OUT="$2"
[ -f "$IN" ] || { echo "ERROR: input not found: $IN" >&2; exit 2; }
for tool in cpio gzip python3; do
  command -v "$tool" >/dev/null || { echo "ERROR: missing required tool: $tool" >&2; exit 2; }
done
case "$OUT" in /*) ;; *) echo "ERROR: output path must be absolute: $OUT" >&2; exit 2 ;; esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> extracting $IN"
(cd "$WORK" && zcat "$IN" | cpio -idm --quiet)
[ -f "$WORK/init" ] || { echo "ERROR: no /init in the archive - is this really an L4T initramfs?" >&2; exit 1; }

echo "==> patching /init partition map (Shield -> mocha)"
python3 - "$WORK/init" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()

repl = {
    'DATAPART':    'DATAPART=/dev/mmcblk0p29',
    'SYSPART':     'SYSPART=/dev/mmcblk0p26',
    'FACTORYPART': 'FACTORYPART=/dev/mmcblk0p18',
    'CALIBPART':   'CALIBPART=/dev/mmcblk0p8',
    'CACHEPART':   'CACHEPART=/dev/mmcblk0p28',
}
n = 0
for k, v in repl.items():
    s2 = re.sub(r'^' + k + r'=.*$', v, s, flags=re.M)
    if s2 != s:
        n += 1
    s = s2
s = re.sub(r'^(\s*)PARTOFF=1\s*$', r'\1PARTOFF=0', s, flags=re.M)

for frag, var in (
    ('$CACHEPART /root/android/cache', 'CACHEPART'),
    ('$SYSPART /root/android/system', 'SYSPART'),
    ('$FACTORYPART /root/mnt/factory', 'FACTORYPART'),
    ('$CALIBPART /root/mnt/usercalib', 'CALIBPART'),
):
    lines = s.split('\n')
    for i, ln in enumerate(lines):
        if frag in ln and not ln.strip().startswith('['):
            lines[i] = '[ -b $' + var + ' ] && ' + ln.strip() + ' 2>/dev/null'
    s = '\n'.join(lines)

if n == 0:
    print("ERROR: no partition definitions matched - unsupported initramfs", file=sys.stderr)
    raise SystemExit(1)
open(p, 'w').write(s)
print('replaced %d/5 partition definitions' % n)
PY

echo "==> repacking"
(cd "$WORK" && find . | cpio -o -H newc --quiet | gzip -9 > "$OUT")
echo "==> done"
ls -l "$OUT"
sha256sum "$OUT"
