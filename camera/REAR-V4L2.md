# Rear IMX179 V4L2 proxy — current state

## What works

- NVC probe: chip ID `0x0002 = 0x8179`, `SET_MODE 3280x2464` succeeds.
- The V4L2 proxy `imx179-v4l2` registers (`imx179-v4l2 2-0011: proxy registered`).
- The tegra VI parses the DTB endpoint and logs
  `creating link for channel vi-output-0`.

## What does not work

- No `/dev/video` node for `vi-output-0` is created.
- Only `/dev/video0` (`vi-output-4`, front OV5693) and `/dev/v4l-subdev0`
  exist.
- `dmesg` stops after `creating link for channel vi-output-0`; the async
  graph never completes for the rear channel.

## Likely cause

The proxy is a plain V4L2 subdev, not a `camera_common` sensor.  The tegra
VI graph expects the sensor subdev to bind through the same async/camera
common path as the OV5693, including `g_mbus_fmt` and the camera_common
entity/pad setup.  The proxy has the format ops but the channel's
`subdev_on_csi` never gets set, so `tegra_channel_init` does not register
the video node.

## Next step

Write a **full V4L2 IMX179 driver** modelled on `drivers/media/i2c/ov5693.c`
(`camera_common_data`, `camera_common_frmfmt`, power rails, mode table
from the NVC driver) instead of the proxy.  That is the 2–4 day port
originally estimated.

## Artifacts

- `camera/src/imx179-v4l2.c` — proxy driver (kept for reference)
- `camera/dts/cam-rear.dts` — rear endpoint/CSI A 4-lane DTB
- `artifacts/kernel-camfix45-uImage` — proxy kernel
- `camera/PROBE-IMX179.md` — hardware probe result
