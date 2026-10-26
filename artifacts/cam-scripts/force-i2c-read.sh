#!/usr/bin/env bash
set -u
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
U=ubuntu@192.168.29.66

cat > /tmp/force.sh <<'EOS'
#!/bin/sh
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null

echo "=== 1) 强制读 0x36（-f 绕过驱动）—— 只看有没有 ACK ==="
timeout 6 i2cget -y -f 2 0x36 2>&1 | head -3

echo ""
echo "=== 2) 强制 dump 0x36 前 16 字节（word 模式 = 16 位寄存器地址）==="
timeout 10 i2cdump -y -f 2 0x36 w 2>&1 | head -8

echo ""
echo "=== 3) 完整 regulator_summary（前 40 行）==="
head -40 /sys/kernel/debug/regulator/regulator_summary 2>/dev/null

echo ""
echo "=== 4) 有没有 cam 相关的导轨 ==="
ls /sys/kernel/debug/regulator/ 2>/dev/null | grep -iE 'cam|1v8|avdd' | head -10
EOS

cat /tmp/force.sh | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/force.sh'
echo ubuntu | sshpass -p ubuntu ssh $PWOPTS "$U" 'sudo -S sh /tmp/force.sh 2>&1' 2>&1 | tail -60
