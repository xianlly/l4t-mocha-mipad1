#!/usr/bin/env bash
set -u
PWOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o PreferredAuthentications=password -o PubkeyAuthentication=no"
U=ubuntu@192.168.29.66

cat > /tmp/cudatest.c <<'EOF'
/* CUDA Driver API feasibility test - no nvcc needed */
#include <stdio.h>
#include <string.h>

typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef unsigned long long CUsize_t;

extern CUresult cuInit(unsigned int);
extern CUresult cuDriverGetVersion(int *);
extern CUresult cuDeviceGetCount(int *);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuDeviceGetName(char *, int, CUdevice);
extern CUresult cuDeviceComputeCapability(int *, int *, CUdevice);
extern CUresult cuDeviceTotalMem(CUsize_t *, CUdevice);
extern CUresult cuCtxCreate(CUcontext *, unsigned int, CUdevice);
extern CUresult cuCtxDestroy(CUcontext);
extern const char *cuGetErrorString(CUresult, const char **);

int main(void)
{
	int ver = 0, count = 0, rc;
	rc = cuInit(0);
	printf("cuInit -> %d\n", rc);
	if (rc) return 1;

	cuDriverGetVersion(&ver);
	printf("CUDA driver version: %d.%d\n", ver / 1000, (ver % 1000) / 10);

	rc = cuDeviceGetCount(&count);
	printf("cuDeviceGetCount -> %d, count=%d\n", rc, count);
	if (rc || count < 1) return 2;

	for (int i = 0; i < count; i++) {
		CUdevice dev;
		char name[256] = {0};
		int major = 0, minor = 0;
		CUsize_t mem = 0;
		CUcontext ctx;

		cuDeviceGet(&dev, i);
		cuDeviceGetName(name, sizeof(name), dev);
		cuDeviceComputeCapability(&major, &minor, dev);
		cuDeviceTotalMem(&mem, dev);
		printf("Device %d: \"%s\"\n", i, name);
		printf("  compute capability: %d.%d\n", major, minor);
		printf("  global memory: %llu MB\n", mem / 1024 / 1024);

		rc = cuCtxCreate(&ctx, 0, dev);
		printf("  cuCtxCreate -> %d\n", rc);
		if (rc == 0) {
			printf("  CONTEXT OK - driver is functional\n");
			cuCtxDestroy(ctx);
		}
	}
	return 0;
}
EOF

cat /tmp/cudatest.c | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/cudatest.c'

cat > /tmp/build.sh <<'EOS'
#!/bin/sh
echo "=== gcc ==="
command -v gcc && gcc --version | head -1
echo ""
echo "=== libcuda 位置 ==="
ls -l /usr/lib/arm-linux-gnueabihf/tegra/libcuda* 2>/dev/null
echo ""
echo "=== 编译测试程序 ==="
gcc -o /tmp/cudatest /tmp/cudatest.c \
    -L/usr/lib/arm-linux-gnueabihf/tegra -lcuda 2>&1 | head -10
ls -l /tmp/cudatest 2>&1
echo ""
echo "=== 运行 ==="
LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf/tegra /tmp/cudatest 2>&1
EOS

cat /tmp/build.sh | sshpass -p ubuntu ssh $PWOPTS "$U" 'cat > /tmp/build.sh'
echo ubuntu | sshpass -p ubuntu ssh $PWOPTS "$U" 'sudo -S sh /tmp/build.sh 2>&1' 2>&1 | tail -35
