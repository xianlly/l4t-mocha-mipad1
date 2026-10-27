#!/usr/bin/env bash
set -u
K=$(cat /tmp/kdir_r241.txt)
TCBIN=$(cat /tmp/tcbin.txt)
cd "$K"
export ARCH=arm
export CROSS_COMPILE="$TCBIN/arm-cortex-linux-gnueabi-"

echo "########## 1) 在 mipi_cal 里加诊断日志 ##########"
python3 - <<'PY'
p = "drivers/media/platform/tegra/camera/channel.c"
s = open(p).read()
old = """static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	unsigned int lanes, cur_lanes, val;
	unsigned int csi_phya, csi_phyb, csi_phya_mask, csi_phyb_mask;
	struct tegra_mc_vi *vi = chan->vi;
	int j;

	lanes = 0;"""
new = """static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	unsigned int lanes, cur_lanes, val;
	unsigned int csi_phya, csi_phyb, csi_phya_mask, csi_phyb_mask;
	struct tegra_mc_vi *vi = chan->vi;
	int j;

	dev_info(chan->vi->dev,
		 "MIPI_CAL: numlanes=%u total_ports=%u port0=%u csia=%d csib=%d bypass=%d\\n",
		 chan->numlanes, chan->total_ports, chan->port[0],
		 chan->csia_io ? 1 : 0, chan->csib_io ? 1 : 0, is_bypass);
	dev_info(chan->vi->dev,
		 "MIPI_CAL: CSI_PHY_CIL_COMMAND_0=0x%08x CSI1=0x%08x\\n",
		 host1x_readl(vi->ndev, CSI_PHY_CIL_COMMAND_0),
		 host1x_readl(vi->ndev, CSI1_PHY_CIL_COMMAND_0));
	dev_info(chan->vi->dev,
		 "MIPI_CAL: CSI_CSI_CIL_A_STATUS=0x%08x B=0x%08x\\n",
		 host1x_readl(vi->ndev, CSI_CSI_CIL_A_STATUS_0),
		 host1x_readl(vi->ndev, CSI_CSI_CIL_B_STATUS_0));

	lanes = 0;"""
if old in s:
    s = s.replace(old, new, 1)
    open(p, "w").write(s)
    print("  mipi_cal logging added")
else:
    print("  FAIL: mipi_cal pattern not found")
PY

echo ""
echo "########## 2) 编译（先看这些宏/字段是否存在） ##########"
LOG=/tmp/r241build24.log
: > "$LOG"
make -j12 HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon" zImage >> "$LOG" 2>&1
rc=$?
echo "build rc=$rc"
if [ "$rc" != "0" ]; then
    grep -nE 'error:|Error [0-9]' "$LOG" | head -12
else
    mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
            -n "SmokeR24.1-camfix14" -d arch/arm/boot/zImage /opt/kbuild/uImage-camfix14 2>&1 | tail -1
    sha256sum /opt/kbuild/uImage-camfix14
fi
