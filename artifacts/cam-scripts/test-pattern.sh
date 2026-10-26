#!/usr/bin/env bash
set -u
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"

find_dev() {
  for ip in 192.168.29.66 192.168.29.99 192.168.29.67; do
    timeout 5 sshpass -p ubuntu ssh $PWOPTS "ubuntu@$ip" 'echo UP' 2>/dev/null | grep -q UP && { echo "ubuntu@$ip"; return; }
  done
}
U=$(find_dev); [ -z "$U" ] && { echo "设备不在线"; exit 1; }
echo "using $U"
S() { sshpass -p ubuntu ssh $PWOPTS "$U" "$@"; }

cat > /tmp/tp.sh <<'EOS'
#!/bin/sh
echo "=== 1) 打开彩条测试模式 ==="
echo 1 > /sys/module/ov5693/parameters/test_mode 2>&1
echo "  test_mode = $(cat /sys/module/ov5693/parameters/test_mode 2>&1)"

echo ""
echo "=== 2) 采集 1 帧（彩条应该在 I420 里可见）==="
rm -f /tmp/tp.jpg /tmp/tp.raw
timeout 40 gst-launch-1.0 -q v4l2src device=/dev/video0 num-buffers=1 ! \
   video/x-raw,format=I420,width=2592,height=1944,framerate=30/1 ! \
   jpegenc ! filesink location=/tmp/tp.jpg 2>&1 | tail -3
ls -l /tmp/tp.jpg 2>&1

echo ""
echo "=== 3) Bayer 原始数据非零检查 ==="
timeout 25 v4l2-ctl -d /dev/video0 --set-fmt-video=width=2592,height=1944,pixelformat=RG10 \
   --stream-mmap --stream-count=1 --stream-to=/tmp/tp.raw >/dev/null 2>&1
ls -l /tmp/tp.raw 2>&1
od -An -tu1 /tmp/tp.raw 2>/dev/null | tr ' ' '\n' | grep -v '^$' | awk '$1!=0{n++} END{print "非零:", n+0, "/", NR}'

echo ""
echo "=== 4) 关掉测试模式 ==="
echo 0 > /sys/module/ov5693/parameters/test_mode 2>&1
echo "  test_mode = $(cat /sys/module/ov5693/parameters/test_mode 2>&1)"

echo ""
echo "=== 5) 日志 ==="
dmesg 2>/dev/null | grep -iE 's_stream|START_STREAM|syncpt|TEST_PATTERN' | tail -8
EOS

cat /tmp/tp.sh | S 'cat > /tmp/tp.sh'
echo ubuntu | S 'sudo -S sh /tmp/tp.sh 2>&1' 2>&1 | tail -30
