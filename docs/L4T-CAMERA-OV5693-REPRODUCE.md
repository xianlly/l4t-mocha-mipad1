# L4T OV5693 相机 bring-up 复现指南

> 目标板：Xiaomi Mi Pad 1（`mocha`，Tegra K1 / Tegra124）
> 系统：L4T R24.1 Ubuntu 14.04（p29 `/l4t`）
> 传感器：前置 **OV5693**（CSI E，1 data lane）
> 结果：**真实图像已出**（1280x720 JPEG 526/546 KB，RAW10 原始帧有效）
> 当前阶段：数据链路已通，剩 Bayer 假色 / 右侧黑边 / ISP 调优

---

## 0. 一句话结论

OV5693 在 L4T 上从“完全黑帧”走到“能出真实场景”，靠的是五个修复：

1. **OV5693 sysfs 寄存器接口** —— 先证明传感器在 streaming。
2. **T124 MIPI 校准** —— 写 `MIPI_CAL_CTRL=0x2a000010`，保持 `clk72mhz`，所有 CIL `SEL=0`。
3. **T124 CSI 寄存器映射** —— L4T R24.1 用的是 T210 布局；CIL E 实际在 `0xa08`，不是 `0x192c`。
4. **CSI E → VI channel 1** —— 数据在 VI CSI 1（`0x200`），不是 VI CSI 4（`0x500`）。
5. **强制 RAW10** —— `IMAGE_DT=0x2b`，word count = `width * 10 / 8`。

---

## 1. 硬件事实（来自小米 Mi Pad 1 原理图）

原理图第 18 页（SoC CAM CSI/DSI）和第 25 页（前后摄像头座）：

```
前置 OV5693（J1502）：
  CSI_E_CLK  -> MIPI_CSI1_CLK_P/M
  CSI_E_D0   -> MIPI_CSI1_LANE0_P/M    只有 1 条 data lane
  CAM2_MCLK / CAM2_PWDN / CAM2_RST_N / CAM_I2C

后置 IMX179（J1501）：
  CSI_A/B    -> MIPI_CSI0_LANE0..3     4 lane
```

因此设备树必须是：

```dts
csi-port = <0x04>;      /* CSI E */
bus-width = <0x01>;     /* 1 lane */
data-lanes = <0x01>;
```

对应现成 DTB：`release/artifacts/dtb-port4-1lane.dtb`。

---

## 2. 根因链（按发现顺序）

### 2.1 传感器侧是好的

`release/patches/ov5693-sysfs-regs.patch` 给驱动加了：

```
/sys/bus/i2c/devices/2-0036/
  status  streaming  exposure  gain  frame_length  regs  reg
```

开流后实测：

```
chip id    : 0x5690 (OV5693)
0x0100     : 0x01 (streaming)
exposure   : 0x07ba0 = 1978.00 lines
gain       : 0x0010 = 1.00x
frame len  : 0x07c0 = 1984 lines
```

结论：传感器供电、I2C、模式表都正常。

### 2.2 MIPI 校准从未真正执行

R24.1 的 T124 分支只做 `STARTCAL`，没写 `MIPI_CAL_CTRL`。
bootloader 留下 `0x2a000000`，MIUI 用的是：

```
0x2a000010 = NOISE_FLT(0xa) | PRESCALE(2) | CLKEN_OVR(1) | AUTOCAL_EN(0)
```

另外 `SELx` 位会让校准挂死，必须全部为 0。

修复后：

```
mipi_dbg: cal done in 6-19 ms, val=0x17a60
```

### 2.3 L4T 的 CSI 寄存器表是 T210 的

R24.1 `camera/registers.h`：

```
PIXEL_PARSER_4_BASE = 0x1838
CIL_OFFSET          = 0xf4       -> CIL E = 0x192c
PHY_CIL_COMMAND     = 0xd0
CLKEN_OVERRIDE      = 0x218
```

T124 实际（MIUI `vi2.c` + 实测 dump）：

```
PHY_CIL_COMMAND      0x908
CLKEN_OVERRIDE       0xaf4
CSI_SW_STATUS_RESET  0xaf0
CIL A                0x92c   (control +8, status +0x10)
CIL B                0x960
CIL C                0x994
CIL D                0x9c8
CIL E                0xa08   (control 0xa10, status 0xa18)
PP A                 0x838
PP B                 0x86c
```

CSIA 之前“看起来能用”只是巧合：`0x838 + 0xd0 == 0x908`，`0x838 + 0xf4 == 0x92c`。
B/C/D/E 全部错位。

### 2.4 CSI E 的数据在 VI channel 1

MIUI 前置预览 dump：

```
VI CSI 1 (0x54080200 区) 有值：IMAGE_SIZE=0x07980a20, IMAGE_DT=0x2b
VI CSI 4 (0x54080500 区) 全 0
```

L4T 原来按 `csi-port=4` 写 `TEGRA_VI_CSI_BASE(4)=0x500`，所以数据到了 CSI 却进不了 VI。
必须映射：

```c
#define TEGRA_CSI_TO_VI_PORT(p)  (((p) >= 4) ? 1 : (p))
```

CSI CIL/PHY 仍然用 port 4；VI CSI block、PP frame start、MW ack done 用 VI channel 1。

### 2.5 默认按 8-bit 收，传感器实际是 RAW10

v4l2 节点默认协商 **RGGB（8-bit）**，于是：

```
IMAGE_DT   = 0x2a (RAW8)
WORD_COUNT = 2592
```

OV5693 实际输出 **RAW10（0x2b）**，所以 pixel parser 报 header error，syncpt 不走。

修复：

```c
data_type = 0x2b;              /* TEGRA_IMAGE_DT_RAW10 */
word_count = width * 10 / 8;   /* 2592 -> 3240, 1280 -> 1600 */
```

---

## 3. 快速复现（推荐：直接用产物）

### 3.1 前提

- 设备已恢复 L4T R24.1 快照，能 SSH（`ubuntu/ubuntu`）。
- U-Boot 从 p29 的 `/l4t/boot` 读取：
  - `uImage-desktop`
  - `mocha-desktop.dtb`
  - `initramfs.cpio.gz`

### 3.2 刷入

```bash
KERNEL=release/artifacts/kernel-camfix38-uImage
DTB=release/artifacts/dtb-port4-1lane.dtb
DEV=ubuntu@192.168.174.66

cat "$KERNEL" | sshpass -p ubuntu ssh "$DEV" 'cat > /tmp/k38'
cat "$DTB"    | sshpass -p ubuntu ssh "$DEV" 'cat > /tmp/d41'

sshpass -p ubuntu ssh "$DEV" 'echo ubuntu | sudo -S sh -c "
  mount | grep -q \" /mnt/p29 \" || { mkdir -p /mnt/p29 && mount /dev/mmcblk0p29 /mnt/p29; }
  cp -f /tmp/k38 /mnt/p29/l4t/boot/uImage-desktop
  cp -f /tmp/d41 /mnt/p29/l4t/boot/mocha-desktop.dtb
  sync
  sha256sum /mnt/p29/l4t/boot/uImage-desktop /mnt/p29/l4t/boot/mocha-desktop.dtb"'

sshpass -p ubuntu ssh "$DEV" 'echo ubuntu | sudo -S reboot'
```

预期 sha256：

| 文件 | sha256 |
|---|---|
| `kernel-camfix38-uImage` | `f14cf806eaf626aaab3643642fc15547fca939622d7bb08b3553a04852a46544` |
| `dtb-port4-1lane.dtb` | `d2004ae01dab350edf5b549f053a7a7c87d136a6082e843cde30a9e1ee93dae5` |

### 3.3 验证

```bash
ssh ubuntu@<IP> 'uname -a'
# 3.10.108_SmokeR24.1-V3.5.1 #44 ... armv7l

cat /sys/bus/i2c/devices/2-0036/status
# chip id 0x5690, 0x0100=0x01, exposure 1978, gain 1.00x

sudo dmesg | grep -E 'MIPI_CAL|cal done|TEGRA_CSI|port0=4'
# MIPI_CAL(T124): port0=4 numlanes=1
# mipi_dbg: cal done in 12 ms

v4l2-ctl -d /dev/video0 --list-formats
# RGGB / RG10 / RG12
```

抓图：

```bash
timeout 20 gst-launch-1.0 -e v4l2src device=/dev/video0 num-buffers=2 ! \
  video/x-raw,width=1280,height=720 ! jpegenc ! filesink location=/tmp/f.jpg
ls -l /tmp/f.jpg
# 526258 / 546393 字节（不再是 30054 的全黑）
```

抓一帧原始 RAW10：

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=720,pixelformat=RG10
timeout 15 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 \
  --stream-to=/tmp/raw10.bin
ls -l /tmp/raw10.bin     # 1843200 = 1280*720*2
```

PC 端 debayer：

```bash
python release/camera-samples/debayer_raw10.py /tmp/raw10.bin
# 生成 raw10-1280-gray.png / raw10-1280-half.png
```

样本已放在 `release/camera-samples/`。

---

## 4. 从源码构建

### 4.1 基线

本项目使用的内核树：

```
/opt/kbuild/r241/SmokeR24.1-kernel-SmokeR24.1-L4T
```

该树已经包含项目之前的 T124 mipical / 板级电源 / 调试改动。
如果从纯净 R24.1 开始，需要先合入项目的 T124 mipical 支持
（`release/patches/t124-mipi-cal.patch` 及对应 `mipi_cal.h`/`Makefile`）。

### 4.2 替换四个文件

把 `release/camera/src/` 下的文件覆盖到内核树：

```bash
K=/opt/kbuild/r241/SmokeR24.1-kernel-SmokeR24.1-L4T
cp camera/src/ov5693.c   $K/drivers/media/i2c/ov5693.c
cp camera/src/mipi_cal.c $K/drivers/media/platform/tegra/mipical/mipi_cal.c
cp camera/src/csi.c      $K/drivers/media/platform/tegra/csi/csi.c
cp camera/src/channel.c  $K/drivers/media/platform/tegra/camera/channel.c
```

### 4.3 编译

```bash
TC=/opt/kbuild/tc/arm-cortex-linux-gnueabi-linaro_4.9-master/bin
export PATH="$TC:$PATH"
export ARCH=arm
export CROSS_COMPILE=arm-cortex-linux-gnueabi-
export LC_ALL=C

cd "$K"
make -j12 zImage HOSTCFLAGS="-Wno-error -fcommon" HOSTLDFLAGS=""

mkimage -A arm -O linux -T kernel -C none \
  -a 0x80008000 -e 0x80008000 \
  -n "SmokeR24.1-camfix38-raw10" \
  -d arch/arm/boot/zImage /opt/kbuild/uImage-camfix38
```

### 4.4 编译 DTB

```bash
dtc -I dts -O dtb -o dtb-port4-1lane.dtb \
  camera/dts/cam-port4-1lane.dts
```

### 4.5 对应补丁（便于 review / 逐步合入）

| 补丁 | 作用 |
|---|---|
| `patches/ov5693-sysfs-regs.patch` | sysfs 寄存器接口 |
| `patches/t124-mipi-cal-ctrl.patch` | `MIPI_CAL_CTRL=0x2a000010` + 保持 clk72 |
| `patches/t124-csi-port-ef.patch` | `tegra_channel_mipi_cal` 支持 PORT_E/F |
| `patches/t124-csi-register-map.patch` | T124 CSI 寄存器映射 |
| `patches/t124-vi-raw10.patch` | CSI E → VI channel 1 + RAW10 |

> 这些 patch 是从项目的**工作树**生成的，建议优先用 `camera/src/` 的完整文件；
> patch 适合对照 diff，不保证能直接应用到纯净 R24.1。

---

## 5. 关键寄存器速查

### 5.1 MIPI_CAL（`0x700e3000`）

```
0x00 CTRL     = 0x2a000010   (NOISE_FLT=0xa, PRESCALE=2, CLKEN_OVR=1)
0x08 STATUS   = 0x00017a60   (bit16 CAL_DONE)
0x58 BIAS0    = 0x00000001   (E_VCLAMP_REF)
0x5c BIAS1    = 0x00000300   (PAD_DRIV_UP_REF=3)
```

### 5.2 CSI / VI（`0x54080000` 起）

```
0x908  PHY_CIL_COMMAND      = 0x12020202   (CSI E, 1 lane)
0xa08  CIL E PAD_CONFIG0    = 0
0xa10  CIL E CONTROL0       = 0x0a
0xa18  CIL E STATUS         = 0x110 / 0
0x86c  PP B INPUT_CONTROL
0x870  PP B CONTROL0
0x87c  PP B COMMAND
0x888  PP B STATUS
0x200  VI CSI 1 区
0x20c  IMAGE_DEF
0x218  IMAGE_SIZE           = 0x07980a20 (2592x1944)
0x21c  IMAGE_SIZE_WC
0x220  IMAGE_DT             = 0x2b (RAW10)
```

### 5.3 sysfs

```bash
cat /sys/bus/i2c/devices/2-0036/status
echo 3501 > /sys/bus/i2c/devices/2-0036/reg
cat /sys/bus/i2c/devices/2-0036/reg
echo "350a 10" > /sys/bus/i2c/devices/2-0036/reg
```

---

## 6. 已验证的结果

| 项目 | 结果 |
|---|---|
| 传感器 streaming | `0x0100=0x01`，曝光/增益/帧长正确 |
| MIPI 校准 | 6–19 ms 完成，`CAL_DONE` |
| CSI E CIL | 无 lane 错误 |
| VI CSI 1 | `IMAGE_SIZE=0x07980a20`，`IMAGE_DT=0x2b` |
| 1280x720 JPEG | 526 / 546 KB，测试图/正常图 MD5 不同 |
| RAW10 | 1,843,200 字节，10-bit 有效 |
| PC debayer | 能看到真实场景 |

---

## 7. 已知限制（尚未修完）

1. **Bayer 假色**
   v4l2 节点只暴露 RGGB/RG10/RG12，没有 RGB/YUV。
   `jpegenc` 把 Bayer 当 YUV/RGB 编码，所以颜色不对。
   → 需要 debayer element 或 Tegra ISP。

2. **右侧黑边**
   v4l2 的 RG10 buffer 是 2 bytes/pixel（2560 字节/行），
   但 VI 每行只写 1600 字节（RAW10 打包），右边 960 字节是黑的。
   → 需要把 `bytesperline`/buffer stride 调到 1600，或改用 packed RAW10 格式。

3. **syncpt timeout 日志**
   仍然会打印，但帧已经进来；属于首帧/同步点条件问题，不影响数据。

---

## 8. 回滚

### 回滚到“相机不可用但系统正常”

```bash
sudo sh -c 'mount /dev/mmcblk0p29 /mnt/p29
  cp /mnt/p29/l4t/boot/mocha-desktop.dtb.bak-cam20 /mnt/p29/l4t/boot/mocha-desktop.dtb
  cp /opt/kbuild/uImage-camfix16 /mnt/p29/l4t/boot/uImage-desktop
  sync; reboot'
```

或使用：

- `release/artifacts/kernel-camfix16-uImage`
- `release/artifacts/dtb-cam20.dtb`

### 从 MIUI 恢复 L4T

使用 `L4T/full-restore-from-miui.sh`（adb → fastboot → RAM installer →
p29 快照 → boot 三件套）。注意：RAM installer 里可能没有 `mkfs.ext4`；
如果 p29 已经是 ext4，脚本会跳过格式化并继续，实测可用。

---

## 9. 文件清单

### 9.1 源码

| 文件 | sha256 |
|---|---|
| `camera/src/ov5693.c` | `f756c3ab9d5c3833a32511a36a99f4eb0ddd9c66e20d2218dc2ab0222a42fc40` |
| `camera/src/mipi_cal.c` | `78a291d32e7498198e43699055de0cc10a57f34758e1dbb3494096ceed89bef2` |
| `camera/src/csi.c` | `2f2e18579ed546d949c3eb984063b6857eabe8b34b690247b42ae85ca510994c` |
| `camera/src/channel.c` | `ac03a368a3987a0df07fee03fe774772d0fd406ef7d0b1cec10ba7a6f2af3e0b` |
| `camera/dts/cam-port4-1lane.dts` | `af0dcf178a9773e69eaa47d064c55d474ee41b2cc9abafd6c186c1a081b2cc13` |

### 9.2 产物

| 文件 | sha256 |
|---|---|
| `artifacts/kernel-camfix38-uImage` | `f14cf806eaf626aaab3643642fc15547fca939622d7bb08b3553a04852a46544` |
| `artifacts/dtb-port4-1lane.dtb` | `d2004ae01dab350edf5b549f053a7a7c87d136a6082e843cde30a9e1ee93dae5` |

### 9.3 参考数据

- `miui-csi/`：MIUI 前置/后置预览时的 CSI/VI 原始 dump
- `reference/miui-vi2.c`：MIUI T124 PCL 驱动（寄存器映射的权威来源）
- `camera-samples/`：成图、RAW10、debayer 脚本
- `patches/`：五个改动的 diff

---

## 10. English Summary

The OV5693 on the Mi Pad 1 is wired to **CSI E with a single data lane**,
not CSI A. Five fixes were required:

1. A sysfs register interface on the OV5693 driver proved the sensor is
   alive and streaming.
2. T124 MIPI calibration must program `MIPI_CAL_CTRL=0x2a000010`
   (`CLKEN_OVR=1`) and keep `clk72mhz` running; all CIL `SEL` bits stay 0.
3. R24.1's CSI register map is the T210 layout. On T124, CIL E lives at
   `0xa08` (not `0x192c`), `PHY_CIL_COMMAND` at `0x908`, `CLKEN_OVERRIDE`
   at `0xaf4`, and PP B at `0x86c`. CSIA only worked by accident.
4. CSI E data is routed to **VI channel 1** (`0x200`), not VI CSI 4
   (`0x500`); map ports E/F to VI channel 1.
5. The sensor outputs RAW10, so force `IMAGE_DT=0x2b` and
   `word_count = width * 10 / 8`.

The result is real 1280x720 frames (526/546 KB JPEG) and valid RAW10 data
that debayers into a visible scene. Remaining work is image quality:
the v4l2 node only exposes Bayer formats (false colours), the RG10 buffer
stride is 2 bytes/pixel while the VI writes 1.25 bytes/pixel (black
strip), and the syncpt timeout log is still printed even though frames
arrive. The complete source, DTB, patches, artifacts and samples are in
this directory.
