#!/usr/bin/env bash
set -u
K=$(cat /tmp/kdir_r241.txt)
cd "$K"
V=drivers/media/i2c/ov5693_mode_tbls.h
P=drivers/media/platform/tegra/ov5693.c

echo "########## 1) v4l2 版 mode_2592x1944 里的 MIPI/PLL 寄存器 ##########"
awk '/mode_2592x1944\[\]/,/^};/' "$V" 2>/dev/null | grep -E '0x4800|0x4801|0x4805|0x460c|0x3010|0x3011|0x3012|0x3013|0x3014|0x0300|0x0301|0x0302' | head -20

echo ""
echo "########## 2) PCL 版 ov5693_2592x1944_i2c 里同样寄存器 ##########"
awk 'NR>=127 && NR<=391' "$P" 2>/dev/null | grep -E '0x4800|0x4801|0x4805|0x460c|0x3010|0x3011|0x3012|0x3013|0x3014|0x0300|0x0301|0x0302' | head -20

echo ""
echo "########## 3) 两表条目数对比 ##########"
echo -n "  v4l2 mode_2592x1944: "; awk '/mode_2592x1944\[\]/,/^};/' "$V" 2>/dev/null | grep -cE '\{0x[0-9a-fA-F]'
echo -n "  PCL ov5693_2592x1944_i2c: "; awk 'NR>=127 && NR<=391' "$P" 2>/dev/null | grep -cE '\{0x[0-9a-fA-F]'
