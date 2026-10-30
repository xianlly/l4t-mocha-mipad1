/*
 * csidump - dump the Tegra K1 VI/CSI register block through /dev/mem.
 *
 * Built statically so it runs on MIUI (bionic) as well as L4T (glibc).
 *
 *   csidump          # 0x54080000 .. 0x540a0000  (VI + CSI A/B/C/D/E/F)
 *   csidump mipi     # 0x700e3000 .. 0x700e4000  (MIPI_CAL)
 *   csidump 0x54080000 0x20000
 *
 * Useful offsets inside the VI block:
 *   CSI PP0  0x838   CIL A  0x92c
 *   CSI PP2  0x1038  CIL C  0x112c
 *   CSI PP4  0x1838  CIL E  0x192c
 *   PHY_CIL_COMMAND for each block: block base + 0xd0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static int dump(unsigned long base, unsigned long len)
{
	int fd;
	void *map;
	unsigned long off;
	unsigned long page = sysconf(_SC_PAGESIZE);
	unsigned long aligned = base & ~(page - 1);
	unsigned long delta = base - aligned;
	unsigned long total = len + delta;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	map = mmap(NULL, total, PROT_READ, MAP_SHARED, fd, aligned);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	printf("# 0x%08lx .. 0x%08lx\n", base, base + len);
	for (off = 0; off < len; off += 4) {
		unsigned int v = *(volatile unsigned int *)((char *)map + delta + off);
		printf("0x%08lx 0x%08x\n", base + off, v);
	}

	munmap(map, total);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "mipi"))
		return dump(0x700e3000UL, 0x1000UL);

	if (argc > 1 && !strcmp(argv[1], "vi1"))
		return dump(0x54084000UL, 0x2000UL);

	if (argc > 2) {
		unsigned long base = strtoul(argv[1], NULL, 0);
		unsigned long len = strtoul(argv[2], NULL, 0);
		if (len == 0 || len > 0x400000) {
			fprintf(stderr, "bad length\n");
			return 2;
		}
		return dump(base, len);
	}

	/* 8 KiB covers VI + CSI A/B (0x838/0x92c), C/D (0x1038/0x112c)
	 * and E/F (0x1838/0x192c).  Reading the whole 0x40000 aperture
	 * can hit unpopulated holes and hang the bus. */
	return dump(0x54080000UL, 0x2000UL);
}
