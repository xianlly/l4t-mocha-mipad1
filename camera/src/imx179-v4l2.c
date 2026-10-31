/*
 * imx179-v4l2.c - minimal V4L2 proxy subdev for the rear IMX179.
 *
 * The NVC driver (drivers/media/platform/tegra/imx179.c) already powers the
 * sensor and programs its 3280x2464 mode table.  This driver only exposes
 * the format and the stream op to the tegra VI/CSI graph, so that a
 * /dev/video node can be created for the rear CSI A/B path.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mediabus.h>

#define IMX179_WIDTH	3280
#define IMX179_HEIGHT	2464

struct imx179_v4l2 {
	struct v4l2_subdev sd;
	struct media_pad pad;
};

static int imx179_v4l2_s_stream(struct v4l2_subdev *sd, int enable)
{
	dev_info(sd->dev, "imx179-v4l2 stream %d (sensor driven by NVC)\n",
		 enable);
	return 0;
}

static int imx179_v4l2_g_mbus_fmt(struct v4l2_subdev *sd,
				  struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = IMX179_WIDTH;
	fmt->height = IMX179_HEIGHT;
	fmt->code = V4L2_MBUS_FMT_SRGGB10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int imx179_v4l2_s_mbus_fmt(struct v4l2_subdev *sd,
				  struct v4l2_mbus_framefmt *fmt)
{
	return imx179_v4l2_g_mbus_fmt(sd, fmt);
}

static int imx179_v4l2_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_fh *fh,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx179_v4l2_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_fh *fh,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index != 0)
		return -EINVAL;
	fse->min_width = IMX179_WIDTH;
	fse->max_width = IMX179_WIDTH;
	fse->min_height = IMX179_HEIGHT;
	fse->max_height = IMX179_HEIGHT;
	return 0;
}

static int imx179_v4l2_enum_frame_interval(struct v4l2_subdev *sd,
					   struct v4l2_subdev_fh *fh,
					   struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index != 0)
		return -EINVAL;
	fie->width = IMX179_WIDTH;
	fie->height = IMX179_HEIGHT;
	fie->interval.numerator = 1;
	fie->interval.denominator = 30;
	return 0;
}

static int imx179_v4l2_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_fh *fh,
			       struct v4l2_subdev_format *fmt)
{
	fmt->format.width = IMX179_WIDTH;
	fmt->format.height = IMX179_HEIGHT;
	fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int imx179_v4l2_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_fh *fh,
			       struct v4l2_subdev_format *fmt)
{
	return imx179_v4l2_get_fmt(sd, fh, fmt);
}

static const struct v4l2_subdev_video_ops imx179_v4l2_video_ops = {
	.s_stream = imx179_v4l2_s_stream,
	.g_mbus_fmt = imx179_v4l2_g_mbus_fmt,
	.s_mbus_fmt = imx179_v4l2_s_mbus_fmt,
};

static const struct v4l2_subdev_pad_ops imx179_v4l2_pad_ops = {
	.enum_mbus_code = imx179_v4l2_enum_mbus_code,
	.enum_frame_size = imx179_v4l2_enum_frame_size,
	.enum_frame_interval = imx179_v4l2_enum_frame_interval,
	.get_fmt = imx179_v4l2_get_fmt,
	.set_fmt = imx179_v4l2_set_fmt,
};

static const struct v4l2_subdev_ops imx179_v4l2_ops = {
	.video = &imx179_v4l2_video_ops,
	.pad = &imx179_v4l2_pad_ops,
};

static int imx179_v4l2_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct imx179_v4l2 *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&priv->sd, client, &imx179_v4l2_ops);
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	priv->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_init(&priv->sd.entity, 1, &priv->pad, 0);
	if (ret < 0) {
		dev_err(&client->dev, "media_entity_init failed %d\n", ret);
		return ret;
	}

	dev_set_drvdata(&client->dev, priv);

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret < 0) {
		dev_err(&client->dev, "async register failed %d\n", ret);
		media_entity_cleanup(&priv->sd.entity);
		return ret;
	}

	dev_info(&client->dev, "imx179-v4l2 proxy registered\n");
	return 0;
}

static int imx179_v4l2_remove(struct i2c_client *client)
{
	struct imx179_v4l2 *priv = dev_get_drvdata(&client->dev);

	v4l2_async_unregister_subdev(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	return 0;
}

static const struct of_device_id imx179_v4l2_of_match[] = {
	{ .compatible = "nvidia,imx179-v4l2" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx179_v4l2_of_match);

static const struct i2c_device_id imx179_v4l2_id[] = {
	{ "imx179-v4l2", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx179_v4l2_id);

static struct i2c_driver imx179_v4l2_driver = {
	.driver = {
		.name = "imx179-v4l2",
		.of_match_table = imx179_v4l2_of_match,
	},
	.probe = imx179_v4l2_probe,
	.remove = imx179_v4l2_remove,
	.id_table = imx179_v4l2_id,
};

module_i2c_driver(imx179_v4l2_driver);

MODULE_DESCRIPTION("IMX179 V4L2 proxy subdev");
MODULE_AUTHOR("mocha L4T camera bring-up");
MODULE_LICENSE("GPL v2");
