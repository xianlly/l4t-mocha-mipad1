#!/usr/bin/env bash
set -u
K=$(cat /tmp/kdir_r241.txt)
cd "$K"
F=drivers/media/i2c/ov5693_mode_tbls.h

echo "########## 1) ov5693_start 表全文（MIPI/PLL 配置都在这里） ##########"
awk '/static const ov5693_reg ov5693_start\[\]/,/^};/' "$F" 2>/dev/null | head -80

echo ""
echo "########## 2) 表里 MIPI/PLL 相关寄存器（0x4800 / 0x3010 / 0x460x / PLL） ##########"
awk '/ov5693_start\[\]/,/^};/' "$F" 2>/dev/null | grep -E '0x300[0-9a-fA-F]|0x301[0-9a-fA-F]|0x480[0-9a-fA-F]|0x460[0-9a-fA-F]|0x0310|0x0311|0x0312|0x0313' | head -30
