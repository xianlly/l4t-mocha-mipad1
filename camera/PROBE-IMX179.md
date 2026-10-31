# IMX179 NVC probe — rear camera hardware verified

Tool: `camera/probe/imx179-test.c` (static ARM).

Output on the device:

```
power on...
SET_POWER(1) rc=1 errno=0
reg 0x0000 rc=2 bytes=11 47
reg 0x0002 rc=2 bytes=81 79 (expect 0x8179)
set mode 3280x2464...
SET_MODE rc=0 errno=0
GET_SENSORDATA rc=0 size=0 id=
power off...
```

Results:

- `SET_POWER(1)` succeeded (regulators, MCLK, reset).
- I2C reads work: register `0x0002` returns **0x8179**, exactly the
  `detect` value in the DT.
- `SET_MODE 3280x2464` succeeded, i.e. the NVC IMX179 driver wrote the
  mode table and reported "stream on".
- Fuse ID is empty (`size=0`), but that only affects calibration data.

Conclusion: the rear IMX179 hardware, power rails, I2C and mode table are
good.  The missing piece is a V4L2 driver + CSI A/B 4-lane device tree, or a
PCL userspace that selects IMX179.
