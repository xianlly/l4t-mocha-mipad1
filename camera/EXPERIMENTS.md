# Image-quality experiments (after camera-ov5693-v1)

Baseline: `camera-ov5693-v1` / camfix38 — real frames, but Bayer false colour,
right-hand dark strip and a regular stripe pattern.

## camfix39 — RG10 16-bit word count

- `word_count = width * 2` for the R16_I container (instead of `width * 10 / 8`).
- Raw frame remains 1,843,200 bytes and debayers into a visible scene.
- Stripes and the dark right edge remain.

## camfix40 — force the sensor mode

- Added the `ov5693.force_mode` module parameter (mode table index).
- `dmesg` confirms `ov5693 mode 3 (s_data->mode 3)`, i.e. the sensor really
  switches to the 1280x720 table instead of staying in 2592x1944.
- A face-down (lens-covered) frame is still **not black**: mean ≈ 282/1023,
  strong FFT peak at a 160-column period.

## camfix41 — RAW10 → R16_I pixel transform

- Set `bypass_pixel_transform = 0` for the 1-lane OV5693 so the VI expands the
  packed RAW10 into the R16_I container.
- The 16-bit interpretation now reaches 16382 (14-bit), but the covered frame
  is still striped.

## Conclusion

The sensor mode switch is working. The remaining pattern is in the
**CSI/VI packing, stride or syncpoint**, not in the scene and not in the sensor
mode. The `frame start syncpt timeout` log also persists, which is consistent
with the VI not latching the frame at the expected line/buffer offset.

Next options:

1. Investigate the T124 VI frame-start syncpoint condition and the
   `IMAGE_SIZE_WC` / `IMAGE_DEF` packing for RAW10.
2. Use the MIUI PCL/ISP path (the vendor pipeline) for clean images.
3. Keep the v1 snapshot as the proof-of-life baseline.

## Artifacts

- `artifacts/kernel-camfix39-uImage`
- `artifacts/kernel-camfix40-uImage`
- `artifacts/kernel-camfix41-uImage`
- `camera-samples/raw-force41.bin` — covered-lens frame from camfix41
- `camera-samples/black-camfix41.png` — contrast-stretched view of it

## Software ISP attempt (option A)

- `camera/isp/software_isp.py` — packed/16-bit detection, black level,
  bilinear RGGB demosaic, gray-world AWB, 3x3 median denoise, gamma.
- `camera/isp/flat_correct.py` — per-column gain from a covered-lens frame.

Result: AWB and denoise improve the colour slightly, but the 160-column
vertical stripe pattern remains.  Flat-field correction with the covered
frame makes it worse because the scene and flat frames came from different
kernel/format configurations.

Conclusion: the stripes are in the raw data (CSI/VI packing or syncpoint),
not in the ISP.  `kernel-camfix43-uImage` (packed RAW10 stride: word count
1600, bytesperline 1600, bypass transform) was built but could not be
deployed because the 7.7 MB SSH transfer kept stalling.  It is the next
targeted test.

## camfix43 — packed RAW10 stride

- Deployed as kernel #49; capture size became 1,152,000 bytes
  (1600 bytes/line x 720), i.e. the packed stride is in effect.
- `v4l2-ctl --get-fmt-video` still reports 2560 because only the try/set
  paths were overridden, but the stream uses 1600.
- The 160-column FFT peak and the bright/striped flat frame remain.
- `test_mode` was confirmed 0 before the capture.

Conclusion: the pattern is **not** a stride gap.  It survives packed
stride, R16_I, bypass and pixel-transform modes, and a covered lens.  It is
below the ISP, in the CSI/VI packing or sensor lane data.  Software ISP
cannot fix it.
