#!/usr/bin/env bash
set -u
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
U=ubuntu@192.168.29.66

cat > /tmp/cuda.sh <<'EOS'
#!/bin/sh
echo "=== 1) CUDA 安装痕迹 ==="
ls -d /usr/local/cuda* 2>/dev/null || echo "  无 /usr/local/cuda"
command -v nvcc 2>/dev/null || echo "  无 nvcc"
ls /usr/lib/arm-linux-gnueabihf/libcuda* 2>/dev/null || echo "  无 libcuda"

echo ""
echo "=== 2) Tegra 图形库（CUDA 依赖） ==="
ls /usr/lib/arm-linux-gnueabihf/tegra/ 2>/dev/null | head -8
ls /usr/lib/arm-linux-gnueabihf/libnvgfx* /usr/lib/arm-linux-gnueabihf/libnvrm* 2>/dev/null | head -6

echo ""
echo "=== 3) GPU 设备节点 ==="
ls /dev/nvhost-* 2>/dev/null | head -10
ls /dev/dri/ 2>/dev/null | head -3

echo ""
echo "=== 4) GPU 频率 / 型号 ==="
cat /sys/kernel/debug/clock/gpu/rate 2>/dev/null || echo "  无 clock debugfs"
dmesg 2>/dev/null | grep -iE 'gk20a|gr3d|gpu' | head -5

echo ""
echo "=== 5) OpenCV / 计算库 ==="
python -c "import cv2; print('cv2', cv2.__version__)" 2>/dev/null || echo "  无 python-opencv"
ls /usr/lib/arm-linux-gnueabihf/libopencv_core* 2>/dev/null | head -2
EOS

cat /tmp/cuda.sh | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/cuda.sh'
echo ubuntu | sshpass -p ubuntu ssh $PWOPTS "$U" 'sudo -S sh /tmp/cuda.sh 2>&1' 2>&1 | tail -35
