#!/usr/bin/env bash
set -u
W=/mnt/c/Users/xiany/Desktop/Pisces-Moze-mocha-debian
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"

find_dev() {
  for ip in 192.168.29.66 192.168.29.99 192.168.29.67; do
    timeout 5 sshpass -p ubuntu ssh $PWOPTS "ubuntu@$ip" 'echo UP' 2>/dev/null | grep -q UP && { echo "ubuntu@$ip"; return; }
  done
  rm -f /tmp/sq.txt
  for i in $(seq 1 254); do ( for t in 1 2; do ping -c1 -W2 "192.168.29.$i" >/dev/null 2>&1 && { echo "192.168.29.$i" >> /tmp/sq.txt; break; }; done ) & done
  wait
  for ip in $(sort -u /tmp/sq.txt 2>/dev/null); do
    timeout 4 sshpass -p ubuntu ssh $PWOPTS "ubuntu@$ip" 'echo UP' 2>/dev/null | grep -q UP && { echo "ubuntu@$ip"; return; }
  done
}

U=$(find_dev); [ -z "$U" ] && { echo "设备不在线"; exit 1; }
echo "using $U"
S() { sshpass -p ubuntu ssh $PWOPTS "$U" "$@"; }

cat /opt/kbuild/uImage-camfix13 | S 'cat > /tmp/k13'
S 'sha256sum /tmp/k13' 2>&1 | tail -1
S 'echo ubuntu | sudo -S sh -c "mount | grep -q /mnt/p29 || mount /dev/mmcblk0p29 /mnt/p29; cp /tmp/k13 /mnt/p29/l4t/boot/uImage-desktop; sync"' 2>&1 | tail -2
S 'echo ubuntu | sudo -S reboot' 2>&1 | tail -1

sleep 20
U2=""
for i in $(seq 1 22); do
  sleep 10; U2=$(find_dev)
  [ -n "$U2" ] && { echo "  up: $U2"; break; }
done
[ -z "$U2" ] && { echo "设备没回来"; exit 1; }
S2() { sshpass -p ubuntu ssh $PWOPTS "$U2" "$@"; }

echo ""
echo "=== 🎯 DPD 解除后采集测试 ==="
cat > /tmp/dpd.sh <<'EOS'
#!/bin/sh
rm -f /tmp/f.jpg /tmp/f.raw
timeout 40 gst-launch-1.0 -q v4l2src device=/dev/video0 num-buffers=1 ! \
   video/x-raw,format=I420,width=2592,height=1944,framerate=30/1 ! \
   jpegenc ! filesink location=/tmp/f.jpg 2>&1 | tail -3
ls -l /tmp/f.jpg 2>&1
echo ""
echo "--- 日志 ---"
dmesg 2>/dev/null | grep -iE 'DPD|s_stream|START_STREAM|syncpt|error setting' | tail -12
echo "--- Bayer 非零检查 ---"
timeout 25 v4l2-ctl -d /dev/video0 --set-fmt-video=width=2592,height=1944,pixelformat=RG10 \
   --stream-mmap --stream-count=1 --stream-to=/tmp/f.raw >/dev/null 2>&1
ls -l /tmp/f.raw 2>&1
od -An -tu1 /tmp/f.raw 2>/dev/null | tr ' ' '\n' | grep -v '^$' | awk '$1!=0{n++} END{print "非零:", n+0, "/", NR}'
EOS
cat /tmp/dpd.sh | S2 'cat > /tmp/dpd.sh'
echo ubuntu | S2 'sudo -S sh /tmp/dpd.sh 2>&1' 2>&1 | tail -22
S2 'cat /tmp/f.jpg' > "$W/L4T/frame-dpd.jpg" 2>/dev/null
ls -l "$W/L4T/frame-dpd.jpg"
