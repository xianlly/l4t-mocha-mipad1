/*
 * imx179.c - minimal PCL camera chip driver for the rear IMX179.
 *
 * The userspace PCL imager (libnvodm_imager) already contains the IMX179
 * mode tables and 3A logic.  The kernel only needs to register a chip
 * whose name matches the DT "chipname" (pcl_IMX179), provide the I2C
 * regmap and handle the rails.
 */

#define CAMERA_DEVICE_INTERNAL

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <media/nvc.h>
#include <media/camera.h>

struct imx179_info {
	struct nvc_regulator vana;
	struct nvc_regulator vdig;
	struct nvc_regulator vif;
};

static int imx179_power_on(struct camera_device *cdev)
{
	struct imx179_info *info = dev_get_drvdata(cdev->dev);
	int err = 0;

	if (cdev->is_power_on)
		return 0;

	if (info->vana.vreg) {
		err = regulator_enable(info->vana.vreg);
		if (err)
			goto out;
	}
	if (info->vdig.vreg) {
		err = regulator_enable(info->vdig.vreg);
		if (err)
			goto out;
	}
	if (info->vif.vreg) {
		err = regulator_enable(info->vif.vreg);
		if (err)
			goto out;
	}

	cdev->is_power_on = 1;
	usleep_range(2000, 2020);

out:
	return err;
}

static int imx179_power_off(struct camera_device *cdev)
{
	struct imx179_info *info = dev_get_drvdata(cdev->dev);
	int err = 0;

	if (!cdev->is_power_on)
		return 0;

	if (info->vif.vreg)
		err = regulator_disable(info->vif.vreg);
	if (info->vdig.vreg)
		err |= regulator_disable(info->vdig.vreg);
	if (info->vana.vreg)
		err |= regulator_disable(info->vana.vreg);

	cdev->is_power_on = 0;
	return err;
}

static int imx179_instance_destroy(struct camera_device *cdev)
{
	struct imx179_info *info = dev_get_drvdata(cdev->dev);

	if (!info)
		return 0;

	dev_set_drvdata(cdev->dev, NULL);

	if (info->vana.vreg)
		regulator_put(info->vana.vreg);
	if (info->vdig.vreg)
		regulator_put(info->vdig.vreg);
	if (info->vif.vreg)
		regulator_put(info->vif.vreg);

	kfree(info);
	return 0;
}

static int imx179_instance_create(struct camera_device *cdev, void *pdata)
{
	struct imx179_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	camera_regulator_get(cdev->dev, &info->vana, "vana");
	camera_regulator_get(cdev->dev, &info->vdig, "vdig");
	camera_regulator_get(cdev->dev, &info->vif, "vif");
	dev_set_drvdata(cdev->dev, info);
	return 0;
}

static int imx179_update(struct camera_device *cdev, void *arg)
{
	return 0;
}

static struct camera_chip imx179_chip = {
	.name = "pcl_IMX179",
	.type = CAMERA_DEVICE_TYPE_I2C,
	.regmap_cfg = {
		.reg_bits = 16,
		.val_bits = 8,
		.cache_type = REGCACHE_NONE,
	},
	.init = imx179_instance_create,
	.release = imx179_instance_destroy,
	.power_on = imx179_power_on,
	.power_off = imx179_power_off,
	.update = imx179_update,
};

static int __init imx179_init(void)
{
	pr_info("%s\n", __func__);
	INIT_LIST_HEAD(&imx179_chip.list);
	camera_chip_add(&imx179_chip);
	return 0;
}
device_initcall(imx179_init);

static void __exit imx179_exit(void)
{
}
module_exit(imx179_exit);

MODULE_DESCRIPTION("IMX179 sensor device");
MODULE_AUTHOR("mocha L4T camera bring-up");
MODULE_LICENSE("GPL v2");
