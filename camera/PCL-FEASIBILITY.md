# PCL feasibility — rear IMX179

Date: 2026-10-30
Status: **kernel side ready, userspace module list is the blocker**

## What is already present

- `CONFIG_VIDEO_IMX179=y` — the L4T kernel already builds the NVC IMX179
  miscdevice driver (`drivers/media/platform/tegra/imx179.c`).
- `/dev/imx179` appears when the V4L2 front DTB is used.
- `/dev/camera.pcl` and `/dev/nvhost-isp*` exist (`CONFIG_VIDEO_CAMERA=y`).
- The DT `tegra-camera-platform` already contains `imx179@2_0010` and
  `ov5693@2_0036` PCL entries.
- `/opt/camtest` contains the glibc PCL userspace:
  `libnvmm_camera_v3.so`, `libnvodm_imager.so`, `libgstnvcamera.so`.
- `libnvodm_imager.so` contains both `sensor_bayer_imx179.c` and
  `sensor_bayer_ov5693.c`.

## Tests

### V4L2 DTB (front camera active)

```
pcl-generic pcl-generic: camera_new_device cannot allocate client:
    pcl_OV5693 bus 2, 36
```

The V4L2 `ov5693` driver owns I2C 2-0036, so the PCL `pcl_OV5693` client
cannot be created. `/dev/imx179` still exists.

### PCL-only DTB (`ov5693@36` disabled)

```
NvCamHwReadLayout: PCLLK_IOCTL_LAYOUT_RD -1 <22>
NvOdmImagerDeviceDetect: PCL layout not empty.
NvPclHwGetModuleList: WARNING: Failed to get module header
NvPclStateControllerOpen: device detection on this platform is not supported
NvPclStateControllerOpen: attempting to use backup default module list
AR0261 ****  Can not open camera device: No such file or directory
ERROR: Failed to power on the sensor.
camera_open failed
```

The PCL library falls back to the **Jetson TK1 default module list**
(AR0261 / IMX135) and never tries IMX179 or OV5693.

## Root cause

- `PCLLK_IOCTL_LAYOUT_RD` returns `EINVAL`, so the kernel layout cannot be
  read.
- `NvPclHwGetModuleList` cannot find a module header (the Mi Pad camera
  modules do not carry the NVIDIA module EEPROM header).
- The fallback module list in the `/opt/camtest` libraries is the generic
  Jetson TK1 list, not the Mi Pad list.
- The MIUI userspace PCL libraries are Android/Bionic and cannot be used
  directly on the glibc L4T rootfs.

The kernel PCL infrastructure and the IMX179 NVC driver are fine; the
missing piece is a **Mi Pad-specific PCL userspace board/module list**.

## Options

1. **Obtain/build a Mi Pad PCL userspace** (module list + `libnvodm_query`)
   for glibc. This is proprietary and not available in the workspace.
2. **Run the MIUI Android PCL userspace in a Bionic chroot/container** on
   L4T, with the L4T kernel NVC drivers. Heavy and uncertain (Binder,
   gralloc, GPU, camera service).
3. **Software ISP on the V4L2 raw path** — debayer + AWB + denoise on the
   host or in a GStreamer element. No PCL dependency. Front raw already
   works; the rear needs a V4L2 `imx179` driver (or a custom NVC app).
4. **Stop at camera-ov5693-v1** — front proof-of-life, documented.

## Recommendation

- Treat the PCL route as **blocked by proprietary userspace** unless a
  Mi Pad PCL userspace becomes available.
- For a clean front image, use the **software ISP** path on the existing
  V4L2 raw stream.
- For the rear, port a **V4L2 imx179 driver** (mode tables from the MIUI
  `imx179.c`) and reuse the same software ISP. The hardware is standard
  4-lane CSI A/B.

## Artifacts

- `camera/src/imx179-pcl.c` — minimal PCL `camera_chip` reference driver
  (not built; kept for a future PCL userspace)
- `camera/dts/cam-pcl.dts` — PCL-only DTB variant
- `camera/PCL-FEASIBILITY.md` — this file
