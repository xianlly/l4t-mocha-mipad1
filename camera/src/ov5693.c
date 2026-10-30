/*
 * ov5693_v4l2.c - ov5693 sensor driver
 *
 * Copyright (c) 2013-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>

#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/camera_common.h>
#include <media/ov5693.h>

#include "cam_dev/camera_gpio.h"

#include <linux/platform/tegra/io-dpd.h>
#include "ov5693_mode_tbls.h"

#define OV5693_MAX_COARSE_DIFF		6

#define OV5693_GAIN_SHIFT		8
#define OV5693_REAL_GAIN_SHIFT		4
#define OV5693_MIN_GAIN		(1 << OV5693_GAIN_SHIFT)
#define OV5693_MAX_GAIN		(16 << OV5693_GAIN_SHIFT)
#define OV5693_MAX_UNREAL_GAIN	(0x0F80)
#define OV5693_MIN_FRAME_LENGTH	(0x0)
#define OV5693_MAX_FRAME_LENGTH	(0x7fff)
#define OV5693_MIN_EXPOSURE_COARSE	(0x0002)
#define OV5693_MAX_EXPOSURE_COARSE	\
	(OV5693_MAX_FRAME_LENGTH-OV5693_MAX_COARSE_DIFF)

#define OV5693_DEFAULT_GAIN		OV5693_MIN_GAIN
#define OV5693_DEFAULT_FRAME_LENGTH	(0x07C0)
#define OV5693_DEFAULT_EXPOSURE_COARSE	\
	(OV5693_DEFAULT_FRAME_LENGTH-OV5693_MAX_COARSE_DIFF)

#define OV5693_DEFAULT_MODE	OV5693_MODE_2592X1944
#define OV5693_DEFAULT_HDR_MODE	OV5693_MODE_2592X1944_HDR
#define OV5693_DEFAULT_WIDTH	2592
#define OV5693_DEFAULT_HEIGHT	1944
#define OV5693_DEFAULT_DATAFMT	V4L2_MBUS_FMT_SRGGB10_1X10
#define OV5693_DEFAULT_CLK_FREQ	24000000
#define OV5693_CHIP_ID		0x5690	/* 0x300a/0x300b */

struct ov5693 {
	struct camera_common_power_rail	power;
	int				numctrls;
	struct v4l2_ctrl_handler	ctrl_handler;
	struct camera_common_eeprom_data eeprom[OV5693_EEPROM_NUM_BLOCKS];
	u8				eeprom_buf[OV5693_EEPROM_SIZE];
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	struct media_pad		pad;

	int				reg_offset;

	s32				group_hold_prev;
	bool				group_hold_en;
	struct regmap			*regmap;
	struct camera_common_data	*s_data;
	struct camera_common_pdata	*pdata;

	/* debug sysfs state */
	u16				reg_addr;
	bool				is_streaming;

	struct v4l2_ctrl		*ctrls[];
};

static struct regmap_config ov5693_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

static int ov5693_g_volatile_ctrl(struct v4l2_ctrl *ctrl);
static int ov5693_s_ctrl(struct v4l2_ctrl *ctrl);
static void ov5693_update_ctrl_range(struct ov5693 *priv, s32 frame_length);

static const struct v4l2_ctrl_ops ov5693_ctrl_ops = {
	.g_volatile_ctrl = ov5693_g_volatile_ctrl,
	.s_ctrl		= ov5693_s_ctrl,
};

static struct v4l2_ctrl_config ctrl_config_list[] = {
/* Do not change the name field for the controls! */
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_GAIN,
		.name = "Gain",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = OV5693_MIN_GAIN,
		.max = OV5693_MAX_GAIN,
		.def = OV5693_DEFAULT_GAIN,
		.step = 1,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_FRAME_LENGTH,
		.name = "Frame Length",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = OV5693_MIN_FRAME_LENGTH,
		.max = OV5693_MAX_FRAME_LENGTH,
		.def = OV5693_DEFAULT_FRAME_LENGTH,
		.step = 1,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_COARSE_TIME,
		.name = "Coarse Time",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = OV5693_MIN_EXPOSURE_COARSE,
		.max = OV5693_MAX_EXPOSURE_COARSE,
		.def = OV5693_DEFAULT_EXPOSURE_COARSE,
		.step = 1,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_COARSE_TIME_SHORT,
		.name = "Coarse Time Short",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.min = OV5693_MIN_EXPOSURE_COARSE,
		.max = OV5693_MAX_EXPOSURE_COARSE,
		.def = OV5693_DEFAULT_EXPOSURE_COARSE,
		.step = 1,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_GROUP_HOLD,
		.name = "Group Hold",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_HDR_EN,
		.name = "HDR enable",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.def = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_EEPROM_DATA,
		.name = "EEPROM Data",
		.type = V4L2_CTRL_TYPE_STRING,
		.flags = V4L2_CTRL_FLAG_VOLATILE,
		.min = 0,
		.max = OV5693_EEPROM_STR_SIZE,
		.step = 2,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_OTP_DATA,
		.name = "OTP Data",
		.type = V4L2_CTRL_TYPE_STRING,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = OV5693_OTP_STR_SIZE,
		.step = 2,
	},
	{
		.ops = &ov5693_ctrl_ops,
		.id = V4L2_CID_FUSE_ID,
		.name = "Fuse ID",
		.type = V4L2_CTRL_TYPE_STRING,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = OV5693_FUSE_ID_STR_SIZE,
		.step = 2,
	},
};

static inline void ov5693_get_frame_length_regs(ov5693_reg *regs,
				u32 frame_length)
{
	regs->addr = OV5693_FRAME_LENGTH_ADDR_MSB;
	regs->val = (frame_length >> 8) & 0xff;
	(regs + 1)->addr = OV5693_FRAME_LENGTH_ADDR_LSB;
	(regs + 1)->val = (frame_length) & 0xff;
}


static inline void ov5693_get_coarse_time_regs(ov5693_reg *regs,
				u32 coarse_time)
{
	regs->addr = OV5693_COARSE_TIME_ADDR_1;
	regs->val = (coarse_time >> 12) & 0xff;
	(regs + 1)->addr = OV5693_COARSE_TIME_ADDR_2;
	(regs + 1)->val = (coarse_time >> 4) & 0xff;
	(regs + 2)->addr = OV5693_COARSE_TIME_ADDR_3;
	(regs + 2)->val = (coarse_time & 0xf) << 4;
}

static inline void ov5693_get_coarse_time_short_regs(ov5693_reg *regs,
				u32 coarse_time)
{
	regs->addr = OV5693_COARSE_TIME_SHORT_ADDR_1;
	regs->val = (coarse_time >> 12) & 0xff;
	(regs + 1)->addr = OV5693_COARSE_TIME_SHORT_ADDR_2;
	(regs + 1)->val = (coarse_time >> 4) & 0xff;
	(regs + 2)->addr = OV5693_COARSE_TIME_SHORT_ADDR_3;
	(regs + 2)->val = (coarse_time & 0xf) << 4;
}

static inline void ov5693_get_gain_regs(ov5693_reg *regs,
				u16 gain)
{
	regs->addr = OV5693_GAIN_ADDR_MSB;
	regs->val = (gain >> 8) & 0xff;

	(regs + 1)->addr = OV5693_GAIN_ADDR_LSB;
	(regs + 1)->val = (gain) & 0xff;
}

static int test_mode;
module_param(test_mode, int, 0644);

static inline int ov5693_read_reg(struct camera_common_data *s_data,
				u16 addr, u8 *val)
{
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;

	return regmap_read(priv->regmap, addr, (unsigned int *) val);
}

static int ov5693_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	int err;
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;

	err = regmap_write(priv->regmap, addr, val);
	if (err)
		pr_err("%s:i2c write failed, %x = %x\n",
			__func__, addr, val);

	return err;
}

static int ov5693_write_table(struct ov5693 *priv,
			      const ov5693_reg table[])
{
	return regmap_util_write_table_8(priv->regmap,
					 table,
					 NULL, 0,
					 OV5693_TABLE_WAIT_MS,
					 OV5693_TABLE_END);
}

static void ov5693_gpio_set(struct ov5693 *priv,
			    unsigned int gpio, int val)
{
	if (priv->pdata->use_cam_gpio)
		cam_gpio_ctrl(priv->i2c_client, gpio, val, 1);
	else {
		if (gpio_cansleep(gpio))
			gpio_set_value_cansleep(gpio, val);
		else
			gpio_set_value(gpio, val);
	}
}

/* mocha port: the camera 1.8 V / 1.2 V rails are GPIO-gated load switches
 * on the PMIC (palmas GPIO1/GPIO2).  The vendor turns them on separately in
 * ardbeg_get_extra_regulators(); enabling the upstream SMPS alone is not
 * enough and leaves the sensor without power. */
static struct tegra_io_dpd mocha_csia_io = {
	.name			= "CSIA",
	.io_dpd_reg_index	= 0,
	.io_dpd_bit		= 0,
};
static struct tegra_io_dpd mocha_csib_io = {
	.name			= "CSIB",
	.io_dpd_reg_index	= 0,
	.io_dpd_bit		= 1,
};
static struct tegra_io_dpd mocha_csie_io = {
	.name			= "CSIE",
	.io_dpd_reg_index	= 1,
	.io_dpd_bit		= 12,
};

static void mocha_csi_dpd_off(void)
{
	tegra_io_dpd_disable(&mocha_csia_io);
	tegra_io_dpd_disable(&mocha_csib_io);
	tegra_io_dpd_disable(&mocha_csie_io);
}

static struct regulator *mocha_cam_1v8;
static struct regulator *mocha_cam_1v2;

static struct regulator *mocha_reg_lookup(const char *underscored,
					  const char *dashed)
{
	struct regulator *r = regulator_get(NULL, underscored);
	if (!IS_ERR_OR_NULL(r))
		return r;
	r = regulator_get(NULL, dashed);
	if (!IS_ERR_OR_NULL(r))
		return r;
	return NULL;
}

static void mocha_cam_extra_regs(struct camera_common_data *s_data, int on)
{
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;
	int err;

	if (on) {
		if (!mocha_cam_1v8)
			mocha_cam_1v8 = mocha_reg_lookup("vdd_cam_1v8",
							 "vdd-cam-1v8");
		if (mocha_cam_1v8) {
			err = regulator_enable(mocha_cam_1v8);
			dev_info(&priv->i2c_client->dev,
				 "vdd_cam_1v8 on (err %d)\n", err);
		} else {
			dev_err(&priv->i2c_client->dev,
				"vdd_cam_1v8 NOT FOUND\n");
		}

		if (!mocha_cam_1v2)
			mocha_cam_1v2 = mocha_reg_lookup("vdd_cam_1v2",
							 "vdd-cam-1v2");
		if (mocha_cam_1v2) {
			err = regulator_enable(mocha_cam_1v2);
			dev_info(&priv->i2c_client->dev,
				 "vdd_cam_1v2 on (err %d)\n", err);
		} else {
			dev_err(&priv->i2c_client->dev,
				"vdd_cam_1v2 NOT FOUND\n");
		}
	} else {
		if (mocha_cam_1v2)
			regulator_disable(mocha_cam_1v2);
		if (mocha_cam_1v8)
			regulator_disable(mocha_cam_1v8);
	}
}

static int ov5693_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power on\n", __func__);

	if (priv->pdata && priv->pdata->power_on) {
		err = priv->pdata->power_on(pw);
		if (err)
			pr_err("%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	/* the CSI pads are held in IO deep power-down; without releasing
	 * them the sensor's data lanes stay dead (vendor does the same in
	 * ardbeg_ov5693_power_on). */
	mocha_csi_dpd_off();
	dev_info(&priv->i2c_client->dev, "CSI IO DPD released\n");

	/* sleeps calls in the sequence below are for internal device
	 * signal propagation as specified by sensor vendor */

	/* mocha/ardbeg vendor order (see ardbeg_ov5693_power_on): hold the
	 * sensor in power-down while the rails come up, then release it. */
	if (pw->pwdn_gpio)
		ov5693_gpio_set(priv, pw->pwdn_gpio, 0);
	if (pw->reset_gpio)
		ov5693_gpio_set(priv, pw->reset_gpio, 0);
	usleep_range(10, 20);

	/* PMIC load switches for 1.8 V / 1.2 V (vendor: ardbeg_get_extra_regulators) */
	mocha_cam_extra_regs(s_data, 1);
	dev_info(&priv->i2c_client->dev, "camera extra rails enabled\n");

	if (0 && pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (err)
			dev_warn(&priv->i2c_client->dev,
				 "dvdd enable failed %d\n", err);
		else
			dev_info(&priv->i2c_client->dev, "dvdd enabled\n");
		err = 0;
	}

	if (pw->avdd)
		err = regulator_enable(pw->avdd);
	if (err)
		goto ov5693_avdd_fail;

	if (pw->iovdd)
		err = regulator_enable(pw->iovdd);
	if (err)
		goto ov5693_iovdd_fail;

	if (pw->pwdn_gpio)
		ov5693_gpio_set(priv, pw->pwdn_gpio, 1);

	/* vendor order: 1.2 V core comes up after PWDN, then RSTN */
	if (pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (!err)
			dev_info(&priv->i2c_client->dev, "dvdd enabled\n");
		else
			dev_warn(&priv->i2c_client->dev,
				 "dvdd enable failed %d\n", err);
		err = 0;
	}

	udelay(2);
	if (pw->reset_gpio)
		ov5693_gpio_set(priv, pw->reset_gpio, 1);

	usleep_range(300, 310);

	pw->state = SWITCH_ON;
	return 0;

ov5693_iovdd_fail:
	regulator_disable(pw->avdd);

ov5693_avdd_fail:
	pr_err("%s failed.\n", __func__);
	return -ENODEV;
}

static int ov5693_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	dev_dbg(&priv->i2c_client->dev, "%s: power off\n", __func__);

	if (priv->pdata && priv->pdata->power_on) {
		err = priv->pdata->power_off(pw);
		if (!err)
			pw->state = SWITCH_OFF;
		else
			pr_err("%s failed.\n", __func__);
		return err;
	}

	/* sleeps calls in the sequence below are for internal device
	 * signal propagation as specified by sensor vendor */

	usleep_range(21, 25);
	if (pw->pwdn_gpio)
		ov5693_gpio_set(priv, pw->pwdn_gpio, 0);
	usleep_range(1, 2);
	if (pw->reset_gpio)
		ov5693_gpio_set(priv, pw->reset_gpio, 0);

	/* datasheet 2.9: reset requires ~2ms settling time*/
	usleep_range(2000, 2010);

	if (pw->iovdd)
		regulator_disable(pw->iovdd);

	if (pw->dvdd)
		regulator_disable(pw->dvdd);
	if (pw->avdd)
		regulator_disable(pw->avdd);

	return 0;
}

static int ov5693_power_put(struct ov5693 *priv)
{
	struct camera_common_power_rail *pw = &priv->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (likely(pw->avdd))
		regulator_put(pw->avdd);

	if (likely(pw->iovdd))
		regulator_put(pw->iovdd);

	pw->avdd = NULL;
	pw->iovdd = NULL;

	if (priv->pdata->use_cam_gpio)
		cam_gpio_deregister(priv->i2c_client, pw->pwdn_gpio);
	else {
		gpio_free(pw->pwdn_gpio);
		gpio_free(pw->reset_gpio);
	}

	return 0;
}

static int ov5693_power_get(struct ov5693 *priv)
{
	struct camera_common_power_rail *pw = &priv->power;
	struct camera_common_pdata *pdata = priv->pdata;
	const char *mclk_name;
	const char *parentclk_name;
	struct clk *parent;
	int err = 0;

	mclk_name = priv->pdata->mclk_name ?
		    priv->pdata->mclk_name : "cam_mclk1";
	pw->mclk = devm_clk_get(&priv->i2c_client->dev, mclk_name);
	if (IS_ERR(pw->mclk)) {
		dev_warn(&priv->i2c_client->dev,
			"no mclk '%s', continuing without it\n", mclk_name);
		pw->mclk = NULL;
	}

	parentclk_name = priv->pdata->parentclk_name;
	if (parentclk_name) {
		parent = devm_clk_get(&priv->i2c_client->dev, parentclk_name);
		if (IS_ERR(parent))
			dev_err(&priv->i2c_client->dev,
				"unable to get parent clcok %s",
				parentclk_name);
		else
			clk_set_parent(pw->mclk, parent);
	}


	/* analog 2.8v */
	err |= camera_common_regulator_get(priv->i2c_client,
			&pw->avdd, pdata->regulators.avdd);
	/* IO 1.8v */
	err |= camera_common_regulator_get(priv->i2c_client,
			&pw->iovdd, pdata->regulators.iovdd);
	/* core rail (optional, mocha port) */
	if (pdata->regulators.dvdd)
		err |= camera_common_regulator_get(priv->i2c_client,
				&pw->dvdd, pdata->regulators.dvdd);

	if (!err) {
		pw->reset_gpio = pdata->reset_gpio;
		pw->pwdn_gpio = pdata->pwdn_gpio;
	}

	if (priv->pdata->use_cam_gpio) {
		err = cam_gpio_register(priv->i2c_client, pw->pwdn_gpio);
		if (err)
			dev_err(&priv->i2c_client->dev,
				"%s ERR can't register cam gpio %u!\n",
				 __func__, pw->pwdn_gpio);
	} else {
		/* mocha port: gpio_request() alone leaves the pin in its default
		 * direction, so gpio_set_value() never drives it and the sensor
		 * stays in power-down.  Configure the direction explicitly. */
		if (gpio_is_valid(pw->pwdn_gpio)) {
			gpio_request(pw->pwdn_gpio, "cam_pwdn_gpio");
			gpio_direction_output(pw->pwdn_gpio, 0);
			dev_info(&priv->i2c_client->dev,
				 "pwdn gpio %u requested as output\n",
				 pw->pwdn_gpio);
		} else {
			dev_err(&priv->i2c_client->dev,
				"invalid pwdn gpio %u\n", pw->pwdn_gpio);
		}
		if (gpio_is_valid(pw->reset_gpio)) {
			gpio_request(pw->reset_gpio, "cam_reset_gpio");
			gpio_direction_output(pw->reset_gpio, 1);
		}
	}


	pw->state = SWITCH_OFF;
	return err;
}

static int ov5693_set_gain(struct ov5693 *priv, s32 val);
static int ov5693_set_frame_length(struct ov5693 *priv, s32 val);
static int ov5693_set_coarse_time(struct ov5693 *priv, s32 val);
static int ov5693_set_coarse_time_short(struct ov5693 *priv, s32 val);

static int force_mode = -1;
module_param(force_mode, int, 0644);
MODULE_PARM_DESC(force_mode, "Force OV5693 mode table index (-1 = auto)");

static int ov5693_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;
	struct v4l2_control control;
	int mode;
	int err;

	dev_info(&client->dev, "ov5693_s_stream(enable=%d)\n", enable);

	dev_dbg(&client->dev, "%s++\n", __func__);

	priv->is_streaming = false;

	if (!enable) {
		ov5693_update_ctrl_range(priv, OV5693_MAX_FRAME_LENGTH);

		return ov5693_write_table(priv,
			mode_table[OV5693_MODE_STOP_STREAM]);
	}

	mode = (force_mode >= 0 && force_mode < ARRAY_SIZE(mode_table)) ?
		force_mode : s_data->mode;
	dev_info(&client->dev, "ov5693 mode %d (s_data->mode %d)\n",
		 mode, s_data->mode);

	err = ov5693_write_table(priv, mode_table[mode]);
	if (err)
		goto exit;

	/* write list of override regs for the asking frame length,
	 * coarse integration time, and gain. Failures to write
	 * overrides are non-fatal */
	control.id = V4L2_CID_GAIN;
	err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
	err |= ov5693_set_gain(priv, control.value);
	if (err)
		dev_dbg(&client->dev, "%s: warning gain override failed\n",
			__func__);

	control.id = V4L2_CID_FRAME_LENGTH;
	err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
	err |= ov5693_set_frame_length(priv, control.value);
	if (err)
		dev_dbg(&client->dev,
			"%s: warning frame length override failed\n",
			__func__);

	control.id = V4L2_CID_COARSE_TIME;
	err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
	err |= ov5693_set_coarse_time(priv, control.value);
	if (err)
		dev_dbg(&client->dev,
			"%s: warning coarse time override failed\n",
			__func__);

	control.id = V4L2_CID_COARSE_TIME_SHORT;
	err = v4l2_g_ctrl(&priv->ctrl_handler, &control);
	err |= ov5693_set_coarse_time_short(priv, control.value);
	if (err)
		dev_dbg(&client->dev,
			"%s: warning coarse time short override failed\n",
			__func__);

	err = ov5693_write_table(priv, mode_table[OV5693_MODE_START_STREAM]);
	dev_info(&client->dev,
		 "START_STREAM table write -> %d (mode %d)\n",
		 err, s_data->mode);
	if (err)
		goto exit;

	priv->is_streaming = true;

	if (test_mode)
		err = ov5693_write_table(priv,
			mode_table[OV5693_MODE_TEST_PATTERN]);

	dev_dbg(&client->dev, "%s--\n", __func__);
	return 0;
exit:
	dev_err(&client->dev, "%s: error setting stream (%d)\n", __func__, err);
	return err;
}

static int ov5693_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;
	struct camera_common_power_rail *pw = &priv->power;

	*status = pw->state == SWITCH_ON;
	return 0;
}

static struct v4l2_subdev_video_ops ov5693_subdev_video_ops = {
	.s_stream	= ov5693_s_stream,
	.s_mbus_fmt	= camera_common_s_fmt,
	.g_mbus_fmt	= camera_common_g_fmt,
	.try_mbus_fmt	= camera_common_try_fmt,
	.enum_mbus_fmt	= camera_common_enum_fmt,
	.g_mbus_config	= camera_common_g_mbus_config,
	.g_input_status = ov5693_g_input_status,
	.enum_framesizes	= camera_common_enum_framesizes,
	.enum_frameintervals	= camera_common_enum_frameintervals,
};

static struct v4l2_subdev_core_ops ov5693_subdev_core_ops = {
	.s_power	= camera_common_s_power,
};

static int ov5693_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_fh *fh,
		struct v4l2_subdev_format *format)
{
	return camera_common_g_fmt(sd, &format->format);
}

static int ov5693_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_fh *fh,
	struct v4l2_subdev_format *format)
{
	int ret;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		ret = camera_common_try_fmt(sd, &format->format);
	else
		ret = camera_common_s_fmt(sd, &format->format);

	return ret;
}

static struct v4l2_subdev_pad_ops ov5693_subdev_pad_ops = {
	.enum_mbus_code = camera_common_enum_mbus_code,
	.set_fmt = ov5693_set_fmt,
	.get_fmt = ov5693_get_fmt,
};

static struct v4l2_subdev_ops ov5693_subdev_ops = {
	.core	= &ov5693_subdev_core_ops,
	.video	= &ov5693_subdev_video_ops,
	.pad	= &ov5693_subdev_pad_ops,
};

static struct of_device_id ov5693_of_match[] = {
	{ .compatible = "nvidia,ov5693", },
	{ },
};

static struct camera_common_sensor_ops ov5693_common_ops = {
	.power_on = ov5693_power_on,
	.power_off = ov5693_power_off,
	.write_reg = ov5693_write_reg,
	.read_reg = ov5693_read_reg,
};

static int ov5693_set_group_hold(struct ov5693 *priv)
{
	int err;
	int gh_prev = switch_ctrl_qmenu[priv->group_hold_prev];

	if (priv->group_hold_en == true && gh_prev == SWITCH_OFF) {
		/* enter group hold */
		err = ov5693_write_reg(priv->s_data,
				       OV5693_GROUP_HOLD_ADDR, 0x01);
		if (err)
			goto fail;

		priv->group_hold_prev = 1;

		dev_dbg(&priv->i2c_client->dev,
			 "%s: enter group hold\n", __func__);
	} else if (priv->group_hold_en == false && gh_prev == SWITCH_ON) {
		/* leave group hold */
		err = ov5693_write_reg(priv->s_data,
				       OV5693_GROUP_HOLD_ADDR, 0x11);
		if (err)
			goto fail;

		err = ov5693_write_reg(priv->s_data,
				       OV5693_GROUP_HOLD_ADDR, 0x61);
		if (err)
			goto fail;

		priv->group_hold_prev = 0;

		dev_dbg(&priv->i2c_client->dev,
			 "%s: leave group hold\n", __func__);
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: Group hold control error\n", __func__);
	return err;
}

static u16 ov5693_to_real_gain(u32 rep, int shift)
{
	u16 gain;
	int gain_int;
	int gain_dec;
	int min_int = (1 << shift);

	if (rep < OV5693_MIN_GAIN)
		rep = OV5693_MIN_GAIN;
	else if (rep > OV5693_MAX_GAIN)
		rep = OV5693_MAX_GAIN;

	gain_int = (int)(rep >> shift);
	gain_dec = (int)(rep & ~(0xffff << shift));

	/* derived from formulat gain = (x * 16 + 0.5) */
	gain = ((gain_int * min_int + gain_dec) * 32 + min_int) / (2 * min_int);

	return gain;
}

static int ov5693_set_gain(struct ov5693 *priv, s32 val)
{
	ov5693_reg reg_list[2];
	int err;
	u16 gain;
	int i;

	if (!priv->group_hold_prev)
		ov5693_set_group_hold(priv);

	/* translate value */
	gain = ov5693_to_real_gain((u32)val, OV5693_GAIN_SHIFT);

	ov5693_get_gain_regs(reg_list, gain);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: gain %04x val: %04x\n", __func__, val, gain);

	for (i = 0; i < 2; i++) {
		err = ov5693_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: GAIN control error\n", __func__);
	return err;
}

static void ov5693_update_ctrl_range(struct ov5693 *priv, s32 frame_length)
{
	struct v4l2_ctrl *ctrl = NULL;
	int ctrl_ids[2] = {V4L2_CID_COARSE_TIME,
			V4L2_CID_COARSE_TIME_SHORT};
	s32 max, min, def;
	int i, j;

	for (i = 0; i < ARRAY_SIZE(ctrl_ids); i++) {
		for (j = 0; j < priv->numctrls; j++) {
			if (priv->ctrls[j]->id == ctrl_ids[i]) {
				ctrl = priv->ctrls[j];
				break;
			}
		}

		if (j == priv->numctrls) {
			dev_err(&priv->i2c_client->dev,
				"could not find ctrl %x\n",
				ctrl_ids[i]);
			continue;
		}

		max = frame_length - OV5693_MAX_COARSE_DIFF;
		/* clamp the value in case above is negative */
		max = clamp_val(max, OV5693_MIN_EXPOSURE_COARSE,
			OV5693_MAX_EXPOSURE_COARSE);
		min = OV5693_MIN_EXPOSURE_COARSE;
		def = clamp_val(OV5693_DEFAULT_EXPOSURE_COARSE, min, max);
		if (__v4l2_ctrl_modify_range(ctrl, min, max, 1, def))
			dev_err(&priv->i2c_client->dev,
				"ctrl %x: range update failed\n",
				ctrl_ids[i]);
	}

}

static int ov5693_set_frame_length(struct ov5693 *priv, s32 val)
{
	ov5693_reg reg_list[2];
	int err;
	u32 frame_length;
	int i;

	if (!priv->group_hold_prev)
		ov5693_set_group_hold(priv);

	frame_length = (u32)val;

	ov5693_get_frame_length_regs(reg_list, frame_length);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %d\n", __func__, frame_length);

	for (i = 0; i < 2; i++) {
		err = ov5693_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	ov5693_update_ctrl_range(priv, val);
	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: FRAME_LENGTH control error\n", __func__);
	return err;
}

static int ov5693_set_coarse_time(struct ov5693 *priv, s32 val)
{
	ov5693_reg reg_list[3];
	int err;
	u32 coarse_time;
	int i;

	if (!priv->group_hold_prev)
		ov5693_set_group_hold(priv);

	coarse_time = (u32)val;

	ov5693_get_coarse_time_regs(reg_list, coarse_time);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %d\n", __func__, coarse_time);

	for (i = 0; i < 3; i++) {
		err = ov5693_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: COARSE_TIME control error\n", __func__);
	return err;
}

static int ov5693_set_coarse_time_short(struct ov5693 *priv, s32 val)
{
	ov5693_reg reg_list[3];
	int err;
	struct v4l2_control hdr_control;
	int hdr_en;
	u32 coarse_time_short;
	int i;

	if (!priv->group_hold_prev)
		ov5693_set_group_hold(priv);

	/* check hdr enable ctrl */
	hdr_control.id = V4L2_CID_HDR_EN;

	err = camera_common_g_ctrl(priv->s_data, &hdr_control);
	if (err < 0) {
		dev_err(&priv->i2c_client->dev,
			"could not find device ctrl.\n");
		return err;
	}

	hdr_en = switch_ctrl_qmenu[hdr_control.value];
	if (hdr_en == SWITCH_OFF)
		return 0;

	coarse_time_short = (u32)val;

	ov5693_get_coarse_time_short_regs(reg_list, coarse_time_short);
	dev_dbg(&priv->i2c_client->dev,
		 "%s: val: %d\n", __func__, coarse_time_short);

	for (i = 0; i < 3; i++) {
		err = ov5693_write_reg(priv->s_data, reg_list[i].addr,
			 reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(&priv->i2c_client->dev,
		 "%s: COARSE_TIME_SHORT control error\n", __func__);
	return err;
}

static int ov5693_eeprom_device_release(struct ov5693 *priv)
{
	int i;

	for (i = 0; i < OV5693_EEPROM_NUM_BLOCKS; i++) {
		if (priv->eeprom[i].i2c_client != NULL) {
			i2c_unregister_device(priv->eeprom[i].i2c_client);
			priv->eeprom[i].i2c_client = NULL;
		}
	}

	return 0;
}

static int ov5693_eeprom_device_init(struct ov5693 *priv)
{
	char *dev_name = "eeprom_ov5693";
	static struct regmap_config eeprom_regmap_config = {
		.reg_bits = 8,
		.val_bits = 8,
	};
	int i;
	int err;

	if (!priv->pdata->has_eeprom)
		return -EINVAL;

	for (i = 0; i < OV5693_EEPROM_NUM_BLOCKS; i++) {
		priv->eeprom[i].adap = i2c_get_adapter(
				priv->i2c_client->adapter->nr);
		memset(&priv->eeprom[i].brd, 0, sizeof(priv->eeprom[i].brd));
		strncpy(priv->eeprom[i].brd.type, dev_name,
				sizeof(priv->eeprom[i].brd.type));
		priv->eeprom[i].brd.addr = OV5693_EEPROM_ADDRESS + i;
		priv->eeprom[i].i2c_client = i2c_new_device(
				priv->eeprom[i].adap, &priv->eeprom[i].brd);

		priv->eeprom[i].regmap = devm_regmap_init_i2c(
			priv->eeprom[i].i2c_client, &eeprom_regmap_config);
		if (IS_ERR(priv->eeprom[i].regmap)) {
			err = PTR_ERR(priv->eeprom[i].regmap);
			ov5693_eeprom_device_release(priv);
			return err;
		}
	}

	return 0;
}

static int ov5693_read_eeprom(struct ov5693 *priv,
				struct v4l2_ctrl *ctrl)
{
	int err, i;

	for (i = 0; i < OV5693_EEPROM_NUM_BLOCKS; i++) {
		err = regmap_bulk_read(priv->eeprom[i].regmap, 0,
			&priv->eeprom_buf[i * OV5693_EEPROM_BLOCK_SIZE],
			OV5693_EEPROM_BLOCK_SIZE);
		if (err)
			return err;
	}

	for (i = 0; i < OV5693_EEPROM_SIZE; i++)
		sprintf(&ctrl->string[i*2], "%02x",
			priv->eeprom_buf[i]);
	return 0;
}

static int ov5693_write_eeprom(struct ov5693 *priv,
				char *string)
{
	int err;
	int i;
	u8 curr[3];
	unsigned long data;

	for (i = 0; i < OV5693_EEPROM_SIZE; i++) {
		curr[0] = string[i*2];
		curr[1] = string[i*2+1];
		curr[2] = '\0';

		err = kstrtol(curr, 16, &data);
		if (err) {
			dev_err(&priv->i2c_client->dev,
				"invalid eeprom string\n");
			return -EINVAL;
		}

		priv->eeprom_buf[i] = (u8)data;
		err = regmap_write(priv->eeprom[i >> 8].regmap,
				   i & 0xFF, (u8)data);
		if (err)
			return err;
		msleep(20);
	}
	return 0;
}

static int ov5693_read_otp_bank(struct ov5693 *priv,
				u8 *buf, int bank, u16 addr, int size)
{
	int err;

	/* sleeps calls in the sequence below are for internal device
	 * signal propagation as specified by sensor vendor */

	usleep_range(10000, 11000);
	err = ov5693_write_table(priv, mode_table[OV5693_MODE_START_STREAM]);
	if (err)
		return err;

	err = ov5693_write_reg(priv->s_data, OV5693_OTP_BANK_SELECT_ADDR,
			       0xC0 | bank);
	if (err)
		return err;

	err = ov5693_write_reg(priv->s_data, OV5693_OTP_LOAD_CTRL_ADDR, 0x01);
	if (err)
		return err;

	usleep_range(10000, 11000);
	err = regmap_bulk_read(priv->regmap, addr, buf, size);
	if (err)
		return err;

	err = ov5693_write_table(priv,
			mode_table[OV5693_MODE_STOP_STREAM]);
	if (err)
		return err;

	return 0;
}

static int ov5693_otp_setup(struct ov5693 *priv)
{
	int err;
	int i;
	struct v4l2_ctrl *ctrl;
	u8 otp_buf[OV5693_OTP_SIZE];

	err = camera_common_s_power(priv->subdev, true);
	if (err)
		return -ENODEV;

	for (i = 0; i < OV5693_OTP_NUM_BANKS; i++) {
		err = ov5693_read_otp_bank(priv,
					&otp_buf[i * OV5693_OTP_BANK_SIZE],
					i,
					OV5693_OTP_BANK_START_ADDR,
					OV5693_OTP_BANK_SIZE);
		if (err) {
			dev_err(&priv->i2c_client->dev,
				"could not read otp bank\n");
			goto ret;
		}
	}

	ctrl = v4l2_ctrl_find(&priv->ctrl_handler, V4L2_CID_OTP_DATA);
	if (!ctrl) {
		dev_err(&priv->i2c_client->dev,
			"could not find device ctrl.\n");
		err = -EINVAL;
		goto ret;
	}

	for (i = 0; i < OV5693_OTP_SIZE; i++)
		sprintf(&ctrl->string[i*2], "%02x",
			otp_buf[i]);
	ctrl->cur.string = ctrl->string;

ret:
	camera_common_s_power(priv->subdev, false);

	return err;
}

static int ov5693_fuse_id_setup(struct ov5693 *priv)
{
	int err;
	int i;
	struct v4l2_ctrl *ctrl;
	u8 fuse_id[OV5693_FUSE_ID_SIZE];

	err = camera_common_s_power(priv->subdev, true);
	if (err)
		return -ENODEV;

	err = ov5693_read_otp_bank(priv,
				&fuse_id[0],
				OV5693_FUSE_ID_OTP_BANK,
				OV5693_FUSE_ID_OTP_START_ADDR,
				OV5693_FUSE_ID_SIZE);
	if (err) {
		dev_err(&priv->i2c_client->dev,
			"could not read otp bank\n");
		goto ret;
	}

	ctrl = v4l2_ctrl_find(&priv->ctrl_handler, V4L2_CID_FUSE_ID);
	if (!ctrl) {
		dev_err(&priv->i2c_client->dev,
			"could not find device ctrl.\n");
		err = -EINVAL;
		goto ret;
	}

	for (i = 0; i < OV5693_FUSE_ID_SIZE; i++)
		sprintf(&ctrl->string[i*2], "%02x",
			fuse_id[i]);
	ctrl->cur.string = ctrl->string;

ret:
	camera_common_s_power(priv->subdev, false);

	return err;
}

static int ov5693_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5693 *priv =
		container_of(ctrl->handler, struct ov5693, ctrl_handler);
	int err = 0;

	if (priv->power.state == SWITCH_OFF)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EEPROM_DATA:
		err = ov5693_read_eeprom(priv, ctrl);
		if (err)
			return err;
		break;
	default:
			pr_err("%s: unknown ctrl id.\n", __func__);
			return -EINVAL;
	}

	return err;
}

static int ov5693_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5693 *priv =
		container_of(ctrl->handler, struct ov5693, ctrl_handler);
	int err = 0;

	if (priv->power.state == SWITCH_OFF)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		err = ov5693_set_gain(priv, ctrl->val);
		break;
	case V4L2_CID_FRAME_LENGTH:
		err = ov5693_set_frame_length(priv, ctrl->val);
		break;
	case V4L2_CID_COARSE_TIME:
		err = ov5693_set_coarse_time(priv, ctrl->val);
		break;
	case V4L2_CID_COARSE_TIME_SHORT:
		err = ov5693_set_coarse_time_short(priv, ctrl->val);
		break;
	case V4L2_CID_GROUP_HOLD:
		if (switch_ctrl_qmenu[ctrl->val] == SWITCH_ON) {
			priv->group_hold_en = true;
		} else {
			priv->group_hold_en = false;
			err = ov5693_set_group_hold(priv);
		}
		break;
	case V4L2_CID_EEPROM_DATA:
		if (!ctrl->string[0])
			break;
		err = ov5693_write_eeprom(priv, ctrl->string);
		if (err)
			return err;
		break;
	case V4L2_CID_HDR_EN:
		break;
	default:
		pr_err("%s: unknown ctrl id.\n", __func__);
		return -EINVAL;
	}

	return err;
}

static int ov5693_ctrls_init(struct ov5693 *priv, bool eeprom_ctrl)
{
	struct i2c_client *client = priv->i2c_client;
	struct camera_common_data *common_data = priv->s_data;
	struct v4l2_ctrl *ctrl;
	int numctrls;
	int err;
	int i;

	dev_dbg(&client->dev, "%s++\n", __func__);

	numctrls = ARRAY_SIZE(ctrl_config_list);
	v4l2_ctrl_handler_init(&priv->ctrl_handler, numctrls);

	for (i = 0; i < numctrls; i++) {
		/* Skip control 'V4L2_CID_EEPROM_DATA' if eeprom inint err */
		if (ctrl_config_list[i].id == V4L2_CID_EEPROM_DATA) {
			if (!eeprom_ctrl) {
				common_data->numctrls -= 1;
				continue;
			}
		}

		ctrl = v4l2_ctrl_new_custom(&priv->ctrl_handler,
			&ctrl_config_list[i], NULL);
		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl\n",
				ctrl_config_list[i].name);
			continue;
		}

		if (ctrl_config_list[i].type == V4L2_CTRL_TYPE_STRING &&
			ctrl_config_list[i].flags & V4L2_CTRL_FLAG_READ_ONLY) {
			ctrl->string = devm_kzalloc(&client->dev,
				ctrl_config_list[i].max + 1, GFP_KERNEL);
			if (!ctrl->string)
				return -ENOMEM;
		}
		priv->ctrls[i] = ctrl;
	}

	priv->numctrls = numctrls;
	priv->subdev->ctrl_handler = &priv->ctrl_handler;
	if (priv->ctrl_handler.error) {
		dev_err(&client->dev, "Error %d adding controls\n",
			priv->ctrl_handler.error);
		err = priv->ctrl_handler.error;
		goto error;
	}

	err = v4l2_ctrl_handler_setup(&priv->ctrl_handler);
	if (err) {
		dev_err(&client->dev,
			"Error %d setting default controls\n", err);
		goto error;
	}

	err = ov5693_otp_setup(priv);
	if (err) {
		/* mocha port: this module has no (readable) OTP bank */
		dev_warn(&client->dev,
			"Error %d reading otp data (ignored)\n", err);
		err = 0;
	}

	err = ov5693_fuse_id_setup(priv);
	if (err) {
		dev_warn(&client->dev,
			"Error %d reading fuse id data (ignored)\n", err);
		err = 0;
	}

	return 0;

error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	return err;
}

MODULE_DEVICE_TABLE(of, ov5693_of_match);

static struct camera_common_pdata *ov5693_parse_dt(struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	int gpio;
	int err;

	if (!node)
		return NULL;

	match = of_match_device(ov5693_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata = devm_kzalloc(&client->dev,
			   sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	err = camera_common_parse_clocks(client, board_priv_pdata);
	if (err) {
		dev_err(&client->dev, "Failed to find clocks\n");
		goto error;
	}

	gpio = of_get_named_gpio(node, "pwdn-gpios", 0);
	if (gpio < 0) {
		dev_err(&client->dev, "pwdn gpios not in DT\n");
		goto error;
	}
	board_priv_pdata->pwdn_gpio = (unsigned int)gpio;

	gpio = of_get_named_gpio(node, "reset-gpios", 0);
	if (gpio < 0) {
		/* reset-gpio is not absoluctly needed */
		dev_dbg(&client->dev, "reset gpios not in DT\n");
		gpio = 0;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	board_priv_pdata->use_cam_gpio =
		of_property_read_bool(node, "cam,use-cam-gpio");

	err = of_property_read_string(node, "avdd-reg",
			&board_priv_pdata->regulators.avdd);
	if (err) {
		dev_err(&client->dev, "avdd-reg not in DT\n");
		goto error;
	}
	err = of_property_read_string(node, "iovdd-reg",
			&board_priv_pdata->regulators.iovdd);
	if (err) {
		dev_err(&client->dev, "iovdd-reg not in DT\n");
		goto error;
	}

	/* mocha port: the OV5693 also needs its 1.2/1.8 V core rail (dvdd);
	 * without it the sensor never answers on i2c. */
	if (of_property_read_string(node, "dvdd-reg",
			&board_priv_pdata->regulators.dvdd))
		board_priv_pdata->regulators.dvdd = NULL;

	board_priv_pdata->has_eeprom =
		of_property_read_bool(node, "has-eeprom");

	return board_priv_pdata;

error:
	devm_kfree(&client->dev, board_priv_pdata);
	return NULL;
}

static int ov5693_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);
	return 0;
}

static const struct v4l2_subdev_internal_ops ov5693_subdev_internal_ops = {
	.open = ov5693_open,
};

static const struct media_entity_operations ov5693_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* -------------------------------------------------------------------------
 * Debug sysfs interface
 *
 * The sensor is only reachable over I2C, so expose the registers that tell
 * us whether it is actually streaming:
 *
 *   reg          generic 16-bit register window (write addr [value])
 *   regs         named dump of the bring-up registers
 *   exposure     exposure in lines      (0x3500/0x3501/0x3502)
 *   gain         gain multiplier        (0x350a/0x350b)
 *   frame_length vertical frame length  (0x380e/0x380f)
 *   streaming    driver state and sensor 0x0100
 *   status       one-shot summary and verdict
 *
 * Usage:
 *   echo 3501 > /sys/bus/i2c/devices/2-0036/reg
 *   cat /sys/bus/i2c/devices/2-0036/reg       -> 0x3501 0x7b
 *   echo "350a 10" > /sys/bus/i2c/devices/2-0036/reg
 *   cat /sys/bus/i2c/devices/2-0036/status
 * ------------------------------------------------------------------------- */

struct ov5693_dbg_state {
	u8	stream;
	u16	chip_id;
	u8	exp_h, exp_m, exp_l;
	u8	gain_h, gain_l;
	u8	fl_h, fl_l;
};

static struct ov5693 *ov5693_from_dev(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct camera_common_data *s_data = to_camera_common_data(client);

	return (struct ov5693 *)s_data->priv;
}

static int ov5693_dbg_read(struct ov5693 *priv, u16 addr, u8 *val)
{
	unsigned int v;
	int err;

	err = regmap_read(priv->regmap, addr, &v);
	if (err)
		return err;

	*val = v & 0xff;
	return 0;
}

static int ov5693_dbg_snapshot(struct ov5693 *priv,
			       struct ov5693_dbg_state *st)
{
	u8 chip_l;
	int err;

	err = ov5693_dbg_read(priv, 0x0100, &st->stream);
	if (err)
		return err;

	err = ov5693_dbg_read(priv, 0x300a, &chip_l);
	if (err)
		return err;
	st->chip_id = chip_l << 8;

	err = ov5693_dbg_read(priv, 0x300b, &chip_l);
	if (err)
		return err;
	st->chip_id |= chip_l;

	err = ov5693_dbg_read(priv, 0x3500, &st->exp_h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x3501, &st->exp_m);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x3502, &st->exp_l);
	if (err)
		return err;

	err = ov5693_dbg_read(priv, 0x350a, &st->gain_h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x350b, &st->gain_l);
	if (err)
		return err;

	err = ov5693_dbg_read(priv, 0x380e, &st->fl_h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x380f, &st->fl_l);
	if (err)
		return err;

	return 0;
}

static ssize_t ov5693_reg_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	u8 val;
	int err;

	err = ov5693_dbg_read(priv, priv->reg_addr, &val);
	if (err)
		return err;

	return sprintf(buf, "0x%04x 0x%02x\n", priv->reg_addr, val);
}

static ssize_t ov5693_reg_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	unsigned int addr, val;
	int n, err;

	n = sscanf(buf, "%x %x", &addr, &val);
	if (n < 1)
		return -EINVAL;
	if (addr > 0xffff || (n == 2 && val > 0xff))
		return -EINVAL;

	priv->reg_addr = addr;
	if (n == 2) {
		err = regmap_write(priv->regmap, addr, val);
		if (err)
			return err;
	}

	return count;
}

struct ov5693_dbg_reg {
	u16		addr;
	const char	*name;
};

static const struct ov5693_dbg_reg ov5693_dbg_regs[] = {
	{ 0x0100, "stream" },
	{ 0x0103, "sw_reset" },
	{ 0x3008, "sys_ctrl0" },
	{ 0x300a, "chip_id_h" },
	{ 0x300b, "chip_id_l" },
	{ 0x3010, "pll1_ctrl" },
	{ 0x3011, "pll1_mult" },
	{ 0x3012, "pll1_sel" },
	{ 0x3013, "pll1_div" },
	{ 0x3014, "pll1_div2" },
	{ 0x3015, "pll1_div3" },
	{ 0x3500, "exp_h" },
	{ 0x3501, "exp_m" },
	{ 0x3502, "exp_l" },
	{ 0x3503, "aec_ctrl" },
	{ 0x350a, "gain_h" },
	{ 0x350b, "gain_l" },
	{ 0x380e, "frame_len_h" },
	{ 0x380f, "frame_len_l" },
	{ 0x4800, "mipi_ctrl" },
	{ 0x4801, "mipi_ctrl01" },
	{ 0x4837, "mipi_timing" },
};

static ssize_t ov5693_regs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	unsigned int i;
	int len = 0;

	for (i = 0; i < ARRAY_SIZE(ov5693_dbg_regs); i++) {
		u8 val;
		int err;

		err = ov5693_dbg_read(priv, ov5693_dbg_regs[i].addr, &val);
		if (err)
			len += sprintf(buf + len,
				"0x%04x %-12s <read error %d>\n",
				ov5693_dbg_regs[i].addr,
				ov5693_dbg_regs[i].name, err);
		else
			len += sprintf(buf + len, "0x%04x %-12s 0x%02x\n",
				ov5693_dbg_regs[i].addr,
				ov5693_dbg_regs[i].name, val);
	}

	return len;
}

static ssize_t ov5693_exposure_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	u8 h, m, l;
	u32 raw;
	int err;

	err = ov5693_dbg_read(priv, 0x3500, &h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x3501, &m);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x3502, &l);
	if (err)
		return err;

	raw = ((h & 0x0f) << 16) | (m << 8) | l;

	return sprintf(buf, "0x%05x %u.%02u lines "
		     "(0x3500=0x%02x 0x3501=0x%02x 0x3502=0x%02x)\n",
		     raw, raw >> 4, (raw & 0xf) * 100 / 16, h, m, l);
}

static ssize_t ov5693_gain_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	u8 h, l;
	u16 raw;
	int err;

	err = ov5693_dbg_read(priv, 0x350a, &h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x350b, &l);
	if (err)
		return err;

	raw = (h << 8) | l;

	return sprintf(buf, "0x%04x %u.%02ux "
		     "(0x350a=0x%02x 0x350b=0x%02x)\n",
		     raw, raw / 16, (raw % 16) * 100 / 16, h, l);
}

static ssize_t ov5693_frame_length_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	u8 h, l;
	u16 fl;
	int err;

	err = ov5693_dbg_read(priv, 0x380e, &h);
	if (err)
		return err;
	err = ov5693_dbg_read(priv, 0x380f, &l);
	if (err)
		return err;

	fl = (h << 8) | l;

	return sprintf(buf, "0x%04x %u lines\n", fl, fl);
}

static ssize_t ov5693_streaming_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	u8 val;
	int err;

	err = ov5693_dbg_read(priv, 0x0100, &val);
	if (err)
		return sprintf(buf, "driver=%d sensor=<read error %d>\n",
			       priv->is_streaming ? 1 : 0, err);

	return sprintf(buf, "driver=%d sensor=0x%02x (%s)\n",
		       priv->is_streaming ? 1 : 0, val,
		       (val & 1) ? "streaming" : "standby");
}

static ssize_t ov5693_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ov5693 *priv = ov5693_from_dev(dev);
	struct camera_common_power_rail *pw = &priv->power;
	struct ov5693_dbg_state st;
	u32 exp_raw;
	u16 gain_raw, frame_len;
	const char *verdict;
	int err, len = 0;

	err = ov5693_dbg_snapshot(priv, &st);
	if (err) {
		len += sprintf(buf + len,
			"ov5693 %s  power=%s\n",
			dev_name(dev), pw->state == SWITCH_ON ? "on" : "off");
		len += sprintf(buf + len,
			"I2C read failed (%d)\n", err);
		if (!priv->is_streaming)
			len += sprintf(buf + len,
				"verdict: IDLE - sensor is powered down or in standby; "
				"start a stream and read again\n");
		else
			len += sprintf(buf + len,
				"verdict: NOT RESPONDING - streaming was requested "
				"but I2C reads fail\n");
		return len;
	}

	exp_raw = ((st.exp_h & 0x0f) << 16) | (st.exp_m << 8) | st.exp_l;
	gain_raw = (st.gain_h << 8) | st.gain_l;
	frame_len = (st.fl_h << 8) | st.fl_l;

	len += sprintf(buf + len,
		"ov5693 %s  power=%s  driver_stream=%d\n",
		dev_name(dev), pw->state == SWITCH_ON ? "on" : "off",
		priv->is_streaming ? 1 : 0);
	len += sprintf(buf + len, "  chip id    : 0x%04x %s\n", st.chip_id,
		(st.chip_id == OV5693_CHIP_ID || st.chip_id == 0x5693)
			? "(OV5693)" : "(unexpected)");
	len += sprintf(buf + len, "  0x0100     : 0x%02x (%s)\n", st.stream,
		(st.stream & 1) ? "streaming" : "standby");
	len += sprintf(buf + len, "  exposure   : 0x%05x = %u.%02u lines\n",
		exp_raw, exp_raw >> 4, (exp_raw & 0xf) * 100 / 16);
	len += sprintf(buf + len, "  gain       : 0x%04x = %u.%02ux\n",
		gain_raw, gain_raw / 16, (gain_raw % 16) * 100 / 16);
	len += sprintf(buf + len, "  frame len  : 0x%04x = %u lines\n",
		frame_len, frame_len);

	if (st.chip_id != OV5693_CHIP_ID && st.chip_id != 0x5693)
		verdict = "UNKNOWN - chip id mismatch";
	else if (!(st.stream & 1))
		verdict = "NOT OUTPUTTING - 0x0100=0, sensor is in standby";
	else if (!priv->is_streaming)
		verdict = "SUSPECT - 0x0100=1 but s_stream(1) never completed";
	else
		verdict = "STREAMING - sensor is in output mode; "
			  "verify MIPI activity on the CSI side";

	len += sprintf(buf + len, "verdict: %s\n", verdict);
	return len;
}

static DEVICE_ATTR(reg, 0644, ov5693_reg_show, ov5693_reg_store);
static DEVICE_ATTR(regs, 0444, ov5693_regs_show, NULL);
static DEVICE_ATTR(exposure, 0444, ov5693_exposure_show, NULL);
static DEVICE_ATTR(gain, 0444, ov5693_gain_show, NULL);
static DEVICE_ATTR(frame_length, 0444, ov5693_frame_length_show, NULL);
static DEVICE_ATTR(streaming, 0444, ov5693_streaming_show, NULL);
static DEVICE_ATTR(status, 0444, ov5693_status_show, NULL);

static struct attribute *ov5693_dbg_attrs[] = {
	&dev_attr_reg.attr,
	&dev_attr_regs.attr,
	&dev_attr_exposure.attr,
	&dev_attr_gain.attr,
	&dev_attr_frame_length.attr,
	&dev_attr_streaming.attr,
	&dev_attr_status.attr,
	NULL,
};

static const struct attribute_group ov5693_dbg_attr_group = {
	.attrs = ov5693_dbg_attrs,
};

static int ov5693_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct camera_common_data *common_data;
	struct device_node *node = client->dev.of_node;
	struct ov5693 *priv;
	char debugfs_name[10];
	int err;

	pr_info("[OV5693]: probing v4l2 sensor.\n");

	if (!IS_ENABLED(CONFIG_OF) || !node)
		return -EINVAL;

	common_data = devm_kzalloc(&client->dev,
			    sizeof(struct camera_common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&client->dev,
			    sizeof(struct ov5693) + sizeof(struct v4l2_ctrl *) *
			    ARRAY_SIZE(ctrl_config_list),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &ov5693_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	priv->pdata = ov5693_parse_dt(client);
	if (!priv->pdata) {
		dev_err(&client->dev, "unable to get platform data\n");
		return -EFAULT;
	}

	common_data->ops		= &ov5693_common_ops;
	common_data->ctrl_handler	= &priv->ctrl_handler;
	common_data->i2c_client		= client;
	common_data->frmfmt		= ov5693_frmfmt;
	common_data->colorfmt		= camera_common_find_datafmt(
					  OV5693_DEFAULT_DATAFMT);
	common_data->power		= &priv->power;
	common_data->ctrls		= priv->ctrls;
	common_data->priv		= (void *)priv;
	common_data->numctrls		= ARRAY_SIZE(ctrl_config_list);
	common_data->numfmts		= ARRAY_SIZE(ov5693_frmfmt);
	common_data->def_mode		= OV5693_DEFAULT_MODE;
	common_data->def_width		= OV5693_DEFAULT_WIDTH;
	common_data->def_height		= OV5693_DEFAULT_HEIGHT;
	common_data->fmt_width		= common_data->def_width;
	common_data->fmt_height		= common_data->def_height;
	common_data->def_clk_freq	= OV5693_DEFAULT_CLK_FREQ;

	priv->i2c_client = client;
	priv->reg_addr = 0x300a;
	priv->s_data			= common_data;
	priv->subdev			= &common_data->subdev;
	priv->subdev->dev		= &client->dev;
	priv->s_data->dev		= &client->dev;

	err = ov5693_power_get(priv);
	if (err)
		return err;

	err = camera_common_parse_ports(client, common_data);
	if (err) {
		dev_err(&client->dev, "Failed to find port info\n");
		return err;
	}
	sprintf(debugfs_name, "ov5693_%c", common_data->csi_port + 'a');
	dev_dbg(&client->dev, "%s: name %s\n", __func__, debugfs_name);
	camera_common_create_debugfs(common_data, debugfs_name);

	v4l2_i2c_subdev_init(priv->subdev, client, &ov5693_subdev_ops);

	/* eeprom interface */
	err = ov5693_eeprom_device_init(priv);
	if (err)
		dev_err(&client->dev,
			"Failed to allocate eeprom reg map: %d\n", err);

	err = ov5693_ctrls_init(priv, !err);
	if (err)
		return err;

	priv->subdev->internal_ops = &ov5693_subdev_internal_ops;
	priv->subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			       V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	priv->subdev->entity.ops = &ov5693_media_ops;
	err = media_entity_init(&priv->subdev->entity, 1, &priv->pad, 0);
	if (err < 0) {
		dev_err(&client->dev, "unable to init media entity\n");
		return err;
	}
#endif

	err = v4l2_async_register_subdev(priv->subdev);
	if (err)
		return err;

	err = sysfs_create_group(&client->dev.kobj, &ov5693_dbg_attr_group);
	if (err)
		dev_warn(&client->dev,
			 "failed to create debug sysfs: %d\n", err);
	else
		dev_info(&client->dev,
			 "debug sysfs ready: reg/regs/exposure/gain/streaming/status\n");

	dev_dbg(&client->dev, "Detected OV5693 sensor\n");


	return 0;
}

static int
ov5693_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(client);
	struct ov5693 *priv = (struct ov5693 *)s_data->priv;

	sysfs_remove_group(&client->dev.kobj, &ov5693_dbg_attr_group);

	v4l2_async_unregister_subdev(priv->subdev);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&priv->subdev->entity);
#endif

	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	ov5693_power_put(priv);
	camera_common_remove_debugfs(s_data);

	return 0;
}

static const struct i2c_device_id ov5693_id[] = {
	{ "ov5693", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ov5693_id);

static struct i2c_driver ov5693_i2c_driver = {
	.driver = {
		.name = "ov5693",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ov5693_of_match),
	},
	.probe = ov5693_probe,
	.remove = ov5693_remove,
	.id_table = ov5693_id,
};

module_i2c_driver(ov5693_i2c_driver);

MODULE_DESCRIPTION("SoC Camera driver for Sony OV5693");
MODULE_AUTHOR("David Wang <davidw@nvidia.com>");
MODULE_LICENSE("GPL v2");

