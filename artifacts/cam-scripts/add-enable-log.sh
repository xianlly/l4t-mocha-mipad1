#!/usr/bin/env bash
set -u
K=$(cat /tmp/kdir_r241.txt)
TCBIN=$(cat /tmp/tcbin.txt)
cd "$K"
export ARCH=arm
export CROSS_COMPILE="$TCBIN/arm-cortex-linux-gnueabi-"

echo "########## 在 tegra_channel_enable_stream 加诊断 ##########"
python3 - <<'PY'
p = "drivers/media/platform/tegra/camera/channel.c"
s = open(p).read()
old = """	/* enable pad power */
	tegra_csi_pad_control(chan->vi->csi, chan->port, ENABLE);"""
new = """	dev_info(chan->vi->dev,
		 "ENABLE_STREAM enter: pg_mode=%d valid_ports=%u port0=%u numlanes=%u\\n",
		 chan->vi->pg_mode, chan->valid_ports, chan->port[0], chan->numlanes);
	/* enable pad power */
	tegra_csi_pad_control(chan->vi->csi, chan->port, ENABLE);"""
if old in s:
    s = s.replace(old, new, 1)
    print("  (a) entry log added")
else:
    print("  (a) FAIL")

old2 = """		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			return ret;"""
new2 = """		ret = tegra_channel_set_stream(chan, true);
		dev_info(chan->vi->dev, "ENABLE_STREAM set_stream(true) -> %d\\n", ret);
		if (ret < 0)
			return ret;"""
if old2 in s:
    s = s.replace(old2, new2, 1)
    print("  (b) set_stream result log added")
else:
    print("  (b) FAIL")

open(p, "w").write(s)
PY

echo ""
echo "########## 编译 ##########"
LOG=/tmp/r241build25.log
: > "$LOG"
make -j12 HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon" zImage >> "$LOG" 2>&1
rc=$?
echo "build rc=$rc"
if [ "$rc" != "0" ]; then grep -nE 'error:|Error [0-9]' "$LOG" | head -10; exit 1; fi
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
        -n "SmokeR24.1-camfix15" -d arch/arm/boot/zImage /opt/kbuild/uImage-camfix15 2>&1 | tail -1
sha256sum /opt/kbuild/uImage-camfix15
