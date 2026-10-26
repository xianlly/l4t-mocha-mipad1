#!/usr/bin/env bash
# Fixed CSI sweep: writes the probe result to a file on the device, then cats it.
set -u
W=/mnt/c/Users/xiany/Desktop/Pisces-Moze-mocha-debian
C="$W/L4T/camwork"
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
LOG=/tmp/sweep2.log
: > "$LOG"

find_dev() {
  for ip in 192.168.29.66 192.168.29.99 192.168.29.67 192.168.29.100; do
    timeout 5 sshpass -p ubuntu ssh $PWOPTS "ubuntu@$ip" 'echo UP' 2>/dev/null | grep -q UP && { echo "ubuntu@$ip"; return; }
  done
  rm -f /tmp/sw2.txt
  for i in $(seq 1 254); do ( for t in 1 2; do ping -c1 -W2 "192.168.29.$i" >/dev/null 2>&1 && { echo "192.168.29.$i" >> /tmp/sw2.txt; break; }; done ) & done
  wait
  for ip in $(sort -u /tmp/sw2.txt 2>/dev/null); do
    timeout 4 sshpass -p ubuntu ssh $PWOPTS "ubuntu@$ip" 'echo UP' 2>/dev/null | grep -q UP && { echo "ubuntu@$ip"; return; }
  done
}

run_case() {
  PORT=$1; LANES=$2; TAG="p${PORT}_l${LANES}"
  echo "=========== CASE $TAG ===========" | tee -a "$LOG"

  python3 - "$C/cam20.dts" "$C/sweep_$TAG.dts" "$PORT" "$LANES" <<'PY'
import sys, re
src, dst, port, lanes = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = open(src).read()
s = re.sub(r'csi-port = <0x0[0-9a-f]>;', f'csi-port = <0x{port:02x}>;', s)
if lanes == 2:
    s = re.sub(r'data-lanes = <[^>]*>;', 'data-lanes = <0x01 0x02>;', s)
    s = re.sub(r'bus-width = <0x0[0-9a-f]>;', 'bus-width = <0x02>;', s)
else:
    s = re.sub(r'data-lanes = <[^>]*>;', 'data-lanes = <0x01 0x02 0x03 0x04>;', s)
    s = re.sub(r'bus-width = <0x0[0-9a-f]>;', 'bus-width = <0x04>;', s)
open(dst, 'w').write(s)
PY
  dtc -I dts -O dtb -o "$C/sweep_$TAG.dtb" "$C/sweep_$TAG.dts" 2>/dev/null
  [ -f "$C/sweep_$TAG.dtb" ] || { echo "  dtb build failed" | tee -a "$LOG"; return; }

  U=$(find_dev)
  [ -z "$U" ] && { echo "  device offline" | tee -a "$LOG"; return; }
  cat "$C/sweep_$TAG.dtb" | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/sd.dtb' 2>/dev/null
  sshpass -p ubuntu ssh $PWOPTS "$U" 'echo ubuntu | sudo -S sh -c "mount | grep -q /mnt/p29 || mount /dev/mmcblk0p29 /mnt/p29; cp /tmp/sd.dtb /mnt/p29/l4t/boot/mocha-desktop.dtb; sync; reboot"' >/dev/null 2>&1

  sleep 25
  U2=""
  for i in $(seq 1 18); do sleep 10; U2=$(find_dev); [ -n "$U2" ] && break; done
  [ -z "$U2" ] && { echo "  no return" | tee -a "$LOG"; return; }

  # probe: write result into a file on the device, then dump it
  sshpass -p ubuntu ssh $PWOPTS "$U2" 'cat > /tmp/pb.sh <<"EOS"
#!/bin/sh
rm -f /tmp/pb.raw /tmp/pbres.txt
timeout 30 v4l2-ctl -d /dev/video0 --set-fmt-video=width=2592,height=1944,pixelformat=RG10 --stream-mmap --stream-count=1 --stream-to=/tmp/pb.raw >/dev/null 2>&1
SZ=$(stat -c%s /tmp/pb.raw 2>/dev/null || echo 0)
NZ=$(od -An -tu1 /tmp/pb.raw 2>/dev/null | tr " " "\n" | grep -v "^$" | awk "\$1!=0{n++} END{print n+0}")
ER=$(dmesg 2>/dev/null | tail -40 | grep -c syncpt)
echo "SIZE=$SZ NONZERO=$NZ SYNC=$ER" > /tmp/pbres.txt
EOS' >/dev/null 2>&1

  sshpass -p ubuntu ssh $PWOPTS "$U2" 'echo ubuntu | sudo -S sh /tmp/pb.sh' >/dev/null 2>&1
  RES=$(sshpass -p ubuntu ssh $PWOPTS "$U2" 'echo ubuntu | sudo -S cat /tmp/pbres.txt' 2>/dev/null | grep SIZE | tail -1)
  echo "  ${RES:-<no result>}" | tee -a "$LOG"
}

for PORT in 0 1 2 3; do
  for LANES in 2 4; do
    run_case $PORT $LANES
  done
done
echo "=========== DONE ===========" | tee -a "$LOG"
