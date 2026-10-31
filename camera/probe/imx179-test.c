/*
 * imx179-test.c - minimal NVC probe for the rear IMX179.
 *
 * Opens /dev/imx179, powers the sensor, reads the chip ID over /dev/i2c-2,
 * programs the 3280x2464 mode and reads the fuse ID.
 *
 * Build (Linaro 4.9, static):
 *   arm-cortex-linux-gnueabi-gcc -static -O2 -o imx179-test imx179-test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define IMX179_IOCTL_SET_MODE		_IOW('o', 1, struct imx179_mode)
#define IMX179_IOCTL_GET_SENSORDATA	_IOR('o', 6, struct imx179_sensordata)
#define IMX179_IOCTL_SET_POWER		_IOW('o', 20, uint32_t)

struct imx179_mode {
	int xres;
	int yres;
	uint32_t frame_length;
	uint32_t coarse_time;
	uint16_t gain;
};

struct imx179_sensordata {
	uint32_t fuse_id_size;
	uint8_t fuse_id[16];
};

static int i2c_read16(int fd, uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	struct i2c_msg msgs[2];
	struct i2c_rdwr_ioctl_data data;
	uint8_t r[2];

	r[0] = (reg >> 8) & 0xff;
	r[1] = reg & 0xff;

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = r;

	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	data.msgs = msgs;
	data.nmsgs = 2;
	return ioctl(fd, I2C_RDWR, &data);
}

int main(void)
{
	int fd, i2c;
	int i;
	uint8_t id[4];
	struct imx179_mode mode;
	struct imx179_sensordata sd;
	int rc;

	fd = open("/dev/imx179", O_RDWR);
	if (fd < 0) {
		perror("open /dev/imx179");
		return 1;
	}

	printf("power on...\n");
	rc = ioctl(fd, IMX179_IOCTL_SET_POWER, 1);
	printf("SET_POWER(1) rc=%d errno=%d\n", rc, errno);
	if (rc < 0)
		goto out;

	sleep(1);

	i2c = open("/dev/i2c-2", O_RDWR);
	if (i2c < 0) {
		perror("open /dev/i2c-2");
	} else {
		memset(id, 0, sizeof(id));
		rc = i2c_read16(i2c, 0x10, 0x0000, id, 2);
		printf("reg 0x0000 rc=%d bytes=%02x %02x\n", rc, id[0], id[1]);
		memset(id, 0, sizeof(id));
		rc = i2c_read16(i2c, 0x10, 0x0002, id, 2);
		printf("reg 0x0002 rc=%d bytes=%02x %02x (expect 0x8179)\n",
		       rc, id[0], id[1]);
		close(i2c);
	}

	memset(&mode, 0, sizeof(mode));
	mode.xres = 3280;
	mode.yres = 2464;
	mode.frame_length = 0x0c00;
	mode.coarse_time = 0x0100;
	mode.gain = 0x0100;
	printf("set mode 3280x2464...\n");
	rc = ioctl(fd, IMX179_IOCTL_SET_MODE, &mode);
	printf("SET_MODE rc=%d errno=%d\n", rc, errno);

	memset(&sd, 0, sizeof(sd));
	rc = ioctl(fd, IMX179_IOCTL_GET_SENSORDATA, &sd);
	printf("GET_SENSORDATA rc=%d size=%u id=", rc, sd.fuse_id_size);
	for (i = 0; i < 16 && i < (int)sd.fuse_id_size; i++)
		printf("%02x", sd.fuse_id[i]);
	printf("\n");

out:
	printf("power off...\n");
	ioctl(fd, IMX179_IOCTL_SET_POWER, 0);
	close(fd);
	return 0;
}
