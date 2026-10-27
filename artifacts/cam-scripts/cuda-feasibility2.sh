#!/usr/bin/env bash
set -u
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
U=ubuntu@192.168.29.66

cat > /tmp/build2.sh <<'EOS'
#!/bin/sh
echo "=== 编译（gnu99）==="
gcc -std=gnu99 -o /tmp/cudatest /tmp/cudatest.c \
    -L/usr/lib/arm-linux-gnueabihf/tegra -lcuda 2>&1 | head -10
ls -l /tmp/cudatest 2>&1
echo ""
echo "=== 运行 ==="
cd /tmp
export LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf/tegra
./cudatest 2>&1
echo "exit=$?"
EOS

cat /tmp/build2.sh | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/build2.sh'
echo ubuntu | sshpass -p ubuntu ssh $PWOPTS "$U" 'sudo -S sh /tmp/build2.sh 2>&1' 2>&1 | tail -25
