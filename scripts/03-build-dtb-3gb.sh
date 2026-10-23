#!/bin/bash
# 03-build-dtb-3gb.sh
# Patch the ORIGINAL tegra124-mocha DTB so the kernel sees 3 GB of RAM.
#
# The stock DTB declares 2 GB. The L4T kernel already has CONFIG_ARM_LPAE=y, so it can
# address the high bank at 4 GiB - only the memory node needs fixing.
#
# Layout: low 2000 MiB + high 1024 MiB, keeping the 48 MiB hole at 4058-4064 MiB free
# for tsec / tzram / lp0_vec / nvdumper:
#   reg = <0x0 0x80000000 0x0 0x7d000000
#          0x1 0x0        0x0 0x40000000>;
#
# Usage: ./03-build-dtb-3gb.sh <orig.dtb> <out.dtb>
set -eu

usage() {
  cat <<'EOF'
Usage: 03-build-dtb-3gb.sh <orig.dtb> <out.dtb>

Enable 3 GB of RAM by rewriting the memory node of the stock mocha device tree.

  memory@0x80000000 {
      device_type = "memory";
      reg = <0x0 0x80000000 0x0 0x7d000000   /* 2000 MiB low  */
             0x1 0x0        0x0 0x40000000>; /* 1024 MiB high */
  };

The node count is checked before and after (stock mocha DTB = 887 nodes), so a broken
edit cannot slip through silently.

Arguments:
  <orig>   original DTB from L4T_R241.zip:root_dir/boot/tegra124-mocha.dtb (144,537 bytes)
  <out>    output file, normally fed to 04-build-dtb-nopmoff.sh

Options:
  -h, --help   show this help

Requires: device-tree-compiler (dtc), python3

NOTE: 2 GB units should skip this step and use the original DTB instead.
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
NODES_BEFORE=$(grep -c '{' "$WORK/in.dts")
echo "==> input nodes: $NODES_BEFORE (stock mocha DTB = 887)"

python3 - "$WORK/in.dts" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'(memory@0x80000000\s*\{)([^}]*)\}', s)
if not m:
    print("ERROR: memory@0x80000000 node not found", file=sys.stderr); raise SystemExit(1)
body = m.group(2)
# device_type already exists in the node - only replace the reg property, otherwise
# dtc rejects the source with a duplicate-property error.
new_reg = ('reg = <0x0 0x80000000 0x0 0x7d000000\n'
           '\t\t       0x1 0x0        0x0 0x40000000>;')
body2 = re.sub(r'reg\s*=\s*<[^>]*>;', new_reg, body)
if body2 == body:
    print("ERROR: no reg property inside the memory node", file=sys.stderr); raise SystemExit(1)
s = s.replace(m.group(0), m.group(1) + body2 + '}')
open(p, 'w').write(s)
print("memory node patched -> 2000 MiB low + 1024 MiB high")
PY

dtc -I dts -O dtb -o "$OUT" "$WORK/in.dts" 2>/dev/null || {
  echo "ERROR: recompiling the DTB failed" >&2; exit 1; }
NODES_AFTER=$(dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -c '{')
echo "==> output nodes: $NODES_AFTER"
[ "$NODES_BEFORE" = "$NODES_AFTER" ] || { echo "ERROR: node count changed ($NODES_BEFORE -> $NODES_AFTER)" >&2; exit 1; }

echo "==> verify memory node:"
dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -A4 'memory@0x80000000' | head -6
echo "==> done"
ls -l "$OUT"
sha256sum "$OUT"
