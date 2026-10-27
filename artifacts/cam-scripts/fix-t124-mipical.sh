#!/usr/bin/env bash
set -u
K=$(cat /tmp/kdir_r241.txt)
TCBIN=$(cat /tmp/tcbin.txt)
cd "$K"
export ARCH=arm
export CROSS_COMPILE="$TCBIN/arm-cortex-linux-gnueabi-"

echo "########## 1) 给 T124 实现真正的 MIPI PHY 校准 ##########"
python3 - <<'PY'
p = "drivers/media/platform/tegra/camera/channel.c"
s = open(p).read()
old = """#else
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	return 0;
}
#endif"""
new = """#else
/*
 * mocha port: on Tegra124 this used to be a stub that did nothing, so the
 * MIPI D-PHY was never calibrated and the CSI receiver could never lock onto
 * the sensor's clock lane.  Port the T210 logic (pick the lane mask from the
 * channel's port and run the calibration) so T124 gets a working PHY too.
 */
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	unsigned int lanes = 0;
	int i;

	dev_info(chan->vi->dev,
		 "MIPI_CAL(T124): port0=%u numlanes=%u total_ports=%u valid_ports=%u bypass=%d\\n",
		 chan->port[0], chan->numlanes, chan->total_ports,
		 chan->valid_ports, is_bypass);

	switch (chan->port[0]) {
	case PORT_A:
		lanes = CSIA;
		break;
	case PORT_B:
		lanes = CSIB;
		break;
	case PORT_C:
		lanes = CSIC;
		break;
	case PORT_D:
		lanes = CSID;
		break;
	default:
		dev_err(chan->vi->dev, "MIPI_CAL: unknown port %u\\n",
			chan->port[0]);
		return -EINVAL;
	}

	dev_info(chan->vi->dev, "MIPI_CAL(T124): lanes=0x%x -> calibrating\\n",
		 lanes);
	return tegra_mipi_calibration(lanes);
}
#endif"""
if old in s:
    s = s.replace(old, new, 1)
    open(p, "w").write(s)
    print("  T124 mipi_cal implemented")
else:
    print("  FAIL: stub pattern not found")
PY

echo ""
echo "########## 2) 编译 ##########"
LOG=/tmp/r241build26.log
: > "$LOG"
make -j12 HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon" zImage >> "$LOG" 2>&1
rc=$?
echo "build rc=$rc"
if [ "$rc" != "0" ]; then
    echo "--- errors ---"
    grep -nE 'error:|Error [0-9]|undeclared' "$LOG" | head -12
    exit 1
fi
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
        -n "SmokeR24.1-camfix16" -d arch/arm/boot/zImage /opt/kbuild/uImage-camfix16 2>&1 | tail -1
sha256sum /opt/kbuild/uImage-camfix16
