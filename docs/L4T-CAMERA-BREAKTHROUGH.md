# L4T 相机攻坚记录（2026-10-26 突破篇）

> 本篇接续 `L4T-CAMERA-INVESTIGATION.md`。记录了从「i2c 完全死寂」到「只差 CSI PHY 一环」的完整突破过程。
> 所有关键突破均来自 **小米官方内核源码 `Xiaomi_Kernel_OpenSource` 的 `mocha-kk-oss` 分支**。

---

## 一、总体成果

| 层级 | 状态 | 说明 |
|---|---|---|
| 内核源码树 | ✅ | `SmokeR24.1-kernel-SmokeR24.1-L4T`（3.10.108），与运行内核完全一致 |
| 编译 / 刷入 / 启动 | ✅ | 全流程打通，可反复迭代（约 15 分钟一轮） |
| OV5693 V4L2 驱动 | ✅ | 编入内核并 probe 成功 |
| 传感器供电 | ✅ | **PMIC GPIO 负载开关（关键突破）** |
| 传感器主时钟 | ✅ | 实测 24.000 MHz |
| i2c 通信 | ✅ | 读 / 写 / 完整 262 条模式表写入全部成功 |
| 媒体控制器图 | ✅ | subdev 绑定、link 创建、`/dev/video0` 就绪 |
| `s_stream` | ✅ | 正常调用，返回 0 |
| CSI IO 深掉电（DPD） | ✅ | 已解除 |
| **MIPI 数据通路** | ❌ | VI 收不到任何帧（连芯片内部彩条都收不到） |

**结论：问题已收敛到 Tegra CSI 接收侧（PHY 无法锁定 clock lane）。**

---

## 二、六个关键突破（均可追溯到官方源码）

### 2.1 GPIO 全部搞反了

```c
// arch/arm/mach-tegra/board-ardbeg.h（官方源码）
#define CAM_RSTN      TEGRA_GPIO_PBB3   /* 219 */
#define CAM2_PWDN     TEGRA_GPIO_PBB6   /* 222 */
#define CAM2_RSTN     TEGRA_GPIO_PCC1   /* 225 */
#define CAM1_PWDN     TEGRA_GPIO_PBB5   /* 221 */
#define CAM_AF_PWDN   TEGRA_GPIO_PBB7   /* 223 */
```

**OV5693（前置）用的是 `CAM2_PWDN`(222) 和 `CAM2_RSTN`(225)。**

我们最初的 DTB 写的是 `pwdn=225 / reset=222` —— **两个引脚完全对调**。后来还错误"修正"成 221（那是 `CAM1_PWDN`，属于后置摄像头）。

### 2.2 完整上电时序

```c
// board-ardbeg-sensors.c: ardbeg_ov5693_power_on()
tegra_io_dpd_disable(&csie_io);          /* 解除 CSI IO 深掉电 */
ardbeg_get_extra_regulators();           /* 打开额外电源轨 */

gpio_set_value(CAM2_PWDN, 0);            /* PWDN 低 */
gpio_set_value(CAM2_RSTN, 0);            /* RSTN 低 */
gpio_set_value(CAM_AF_PWDN, 0);
usleep_range(10, 20);

regulator_enable(ov5693_afvdd);
regulator_enable(pw->avdd);              /* 2.7V */
regulator_enable(ov5693_1v8);            /* 1.8V */
gpio_set_value(CAM2_PWDN, 1);            /* PWDN 高 */
regulator_enable(ov5693_1v2);            /* 1.2V 核心 */
udelay(2);
gpio_set_value(CAM2_RSTN, 1);            /* RSTN 高 */
usleep_range(300, 310);
```

**关键点：PWDN 和 RSTN 必须在电源轨之前拉低。**

### 2.3 ⭐ PMIC GPIO 负载开关（本次最大突破）

```c
// board-ardbeg-sensors.c: ardbeg_get_extra_regulators()
ov5693_1v2   = regulator_get(NULL, "vdd_cam_1v2");
ov5693_1v8   = regulator_get(NULL, "vdd_cam_1v8");
ov5693_afvdd = regulator_get(NULL, "imx179_reg1");
```

对应的 DT 定义：

```dts
vdd_cam_1v8: regulator@10 {
    regulator-name = "vdd-cam-1v8";
    regulator-min-microvolt = <1800000>;
    enable-active-high;
    gpio = <&palmas_gpio 1 0>;      /* ← PMIC 的 GPIO1 控制的负载开关！ */
    vin-supply = <&palmas_smps8>;
};

vdd_cam_1v2: regulator@11 {
    regulator-name = "vdd-cam-1v2";
    regulator-min-microvolt = <1200000>;
    enable-active-high;
    gpio = <&palmas_gpio 2 0>;      /* ← PMIC 的 GPIO2 */
    vin-supply = <&palmas_smps8>;
};
```

**相机的 1.8V / 1.2V 不是直接从 PMIC SMPS 出来的，而是经过两个由 PMIC GPIO 控制的负载开关。**

只打开上游 SMPS 是不够的 —— 开关不打开，传感器完全没有电，i2c 因此完全死寂。

这一条是所有供电修复中最关键的，也是排查时间最久的一条。

### 2.4 主时钟名

```
Tegra12（T124）的相机时钟在 CCF 里叫 "mclk" / "mclk2"
（不是 Tegra21 的 "cam_mclk1"）

// drivers/platform/tegra/tegra12_clocks.c
.name = "mclk",
.name = "mclk2",
CLK_DUPLICATE("mclk", NULL, "default_mclk"),
```

DT 里写 `mclk = "mclk2"` 后实测 `clk_get_rate()` 返回 **24,000,000 Hz** ✓

### 2.5 `gpio_request()` 之后必须 `gpio_direction_output()`

原驱动只有：

```c
gpio_request(pw->pwdn_gpio, "cam_pwdn_gpio");
gpio_request(pw->reset_gpio, "cam_reset_gpio");
```

**没有设置方向 → 引脚保持默认（输入）状态 → `gpio_set_value()` 静默无效。**

补上 `gpio_direction_output()` 后才有实际电平输出。

### 2.6 CSI IO 深掉电（DPD）

```c
static struct tegra_io_dpd csia_io = { "CSIA", .io_dpd_reg_index = 0, .io_dpd_bit = 0  };
static struct tegra_io_dpd csib_io = { "CSIB", .io_dpd_reg_index = 0, .io_dpd_bit = 1  };
static struct tegra_io_dpd csie_io = { "CSIE", .io_dpd_reg_index = 1, .io_dpd_bit = 12 };
```

上电前必须 `tegra_io_dpd_disable()` 三个都解除。

---

## 三、电源轨权威映射（官方源码 vs 我们的 DTB）

```
官方 tegra124-tn8-pmic-e1736-1000-a00.dtsi:
  dvdd        <- vdd-lcd-io
  vdig        <- vdd-lcd-io
  avdd_ov5693 <- avdd-cam
  dovdd       <- vdd-cam-af
  vif         <- vdd-cam-af
  vana        <- avdd-cam-af
```

我们 DTB 里这张 consumer 表与官方**完全一致**，说明 DTB 来源正确。

---

## 四、媒体控制器图（已完全建立）

```
vi vi: parsing node /host1x/vi
vi vi: handling endpoint /host1x/vi/ports/port@0/endpoint
vi vi: parsing node /i2c@7000c500/ov5693@36
vi vi: handling endpoint /i2c@7000c500/ov5693@36/ports/port@0/endpoint
vi vi: subdev ov5693 2-0036 bound
vi vi: notify complete, all subdevs registered
vi vi: creating links for entity ov5693 2-0036
vi vi: processing endpoint /i2c@7000c500/ov5693@36/ports/port@0/endpoint
vi vi: skipping channel port /i2c@7000c500/ov5693@36:0    ← 正常（从 VI 侧处理）
vi vi: creating links for channels
vi vi: creating link for channel vi-output-0
vi vi: creating ov5693 2-0036:0 -> vi-output-0:0 link

/dev/v4l-subdev0 = "ov5693 2-0036"
/dev/video0      = "vi-output-0, ov5693 2-0036"
```

`s_stream` 调用链：

```
[59.566472] ov5693 2-0036: ov5693_s_stream(enable=1)
[59.592915] ov5693 2-0036: START_STREAM table write -> 0 (mode 0)
[59.612384] video4linux video0: frame start syncpt timeout!0
```

**⚠️ 重要陷阱：`is_streaming` 会卡住。** 如果用 `pkill` 强杀采集进程，`tegra_channel_set_stream(chan, false)` 不会执行，`chan->is_streaming` 会一直停留在 1，之后每次开流都会在 `if (atomic_read(&chan->is_streaming) == on) return 0;` 处提前返回，**再也不会调用 `s_stream`**。

**排查任何"s_stream 没被调用"的问题前，必须先重启一次取得干净状态。**

---

## 五、模式表核对（推翻了一个错误假设）

一开始怀疑 v4l2 驱动缺模式表，因为 `ov5693_start[]` 只有一条 `{0x0100, 0x01}`。

**实际核对结果：模式表是完整的，而且和原厂 PCL 版逐字节一致。**

```
drivers/media/i2c/ov5693_mode_tbls.h:      mode_2592x1944[]        262 条寄存器
drivers/media/platform/tegra/ov5693.c:     ov5693_2592x1944_i2c[]  262 条寄存器

两者的 MIPI/PLL 部分完全相同：
  {0x3011, 0x21}, {0x3012, 0x09}, {0x3013, 0x10}, {0x3014, 0x00}
  {0x4800, 0x20},  /* dis-continuous clock, 2-lane */
```

`ov5693_start[]` 只有一条是**正常设计** —— 模式表在 `s_stream` 里先写：

```c
err = ov5693_write_table(priv, mode_table[s_data->mode]);            /* 完整配置 */
err = ov5693_write_table(priv, mode_table[OV5693_MODE_START_STREAM]); /* 开流 */
```

**所以传感器配置是对的，问题不在传感器侧。**

---

## 六、CSI 端口 / lane 穷举（已完全排除）

自动化脚本 `L4T/sweep-csi2.sh` 测试了 8 种组合：

| csi-port | data-lanes | 结果 |
|---|---|---|
| 0 | 2 | SIZE=10077696 NONZERO=1 SYNC=3 |
| 0 | 4 | SIZE=10077696 NONZERO=1 SYNC=3 |
| 1 | 2 | SIZE=10077696 NONZERO=1 SYNC=4 |
| 1 | 4 | SIZE=10077696 NONZERO=1 SYNC=3 |
| 2 | 2 | SIZE=10077696 NONZERO=1 SYNC=4 |
| 2 | 4 | SIZE=10077696 NONZERO=1 SYNC=3 |
| 3 | 2 | SIZE=10077696 NONZERO=1 SYNC=4 |
| 3 | 4 | （同上） |

**结论：不是 CSI 端口问题，也不是 lane 数问题。**

---

## 七、彩条测试（决定性诊断）

驱动里有模块参数：

```c
static int test_mode;
module_param(test_mode, int, 0644);        /* /sys/module/ov5693/parameters/test_mode */
```

打开后 `s_stream` 会额外写 `tp_colorbars`（芯片内部彩条生成器）。

```bash
echo 1 > /sys/module/ov5693/parameters/test_mode
# 采集 → 仍然全零 + syncpt timeout
```

**这是最关键的证据**：连传感器自己产生的彩条都到不了 VI，说明 **MIPI 物理链路完全没建立**，与镜头/曝光/模拟部分无关。

---

## 八、为什么 v4l2 路径在这块板子上走不通

三个独立来源都证明 **mocha 的相机从出厂起只用 PCL 路径**：

| 来源 | 证据 |
|---|---|
| MIUI9 原厂 DTB | 无任何 v4l2 相机节点（只有 regulator consumer） |
| MoKee Q (Android 10) device tree | `TARGET_KERNEL_SOURCE := kernel/xiaomi/mocha-R21.8`；`nvcamera.conf` 用 `/dev/ov5693`（PCL 设备节点） |
| SmokeR21.5 / SmokeR24.1 DTS | 无 v4l2 相机节点 |

**MIPI PHY 的 lane 初始化在 NVIDIA 的 ODM/PCL 私有代码里**，v4l2 的 VI 驱动做不到同样的配置，所以数据链路建立不起来。

---

## 九、PCL 路径的障碍（已验证）

从 MIUI9 的 `system.img` 提取出的完整 PCL 用户空间：

```
libnvmm_camera.so        640 KB
libnvmm_camera_v3.so     1.4 MB
camera.tegra.so          1.1 MB
nvcameratools.py          80 KB    ← Python 客户端！
nvcamera_server.py       6.9 KB
nvcamera_v1.py            47 KB    ← SWIG 封装
_nvcamera_v1.so          103 KB
```

**但全部是 Android(bionic) 编译的：**

```
libnvmm_camera.so  interpreter = /system/bin/linker   ← Android 链接器，glibc 上加载不了
_nvcamera_v1.so    依赖 libnvos/libnvmm/libnvrm/libnvomx/libstlport ...
camera.tegra.so    依赖 libbinder/libcamera_client/libmedia/libui/libcutils ...
```

**结论：原厂 PCL blob 无法在 Ubuntu 上运行。**

---

## 十、剩余问题与下一步

### 现状

```
✅ 传感器侧：完全正常（供电、时钟、i2c、262 条配置寄存器全部写入）
✅ 内核相机框架：媒体图、subdev、s_stream、VI 通道全部建立
❌ Tegra CSI 接收侧：PHY 收不到 clock lane / 数据
```

### 下一步（按性价比）

| 顺序 | 动作 | 成本 | 说明 |
|---|---|---|---|
| **1** | 加 CSI PHY 状态 dump | 40 分钟 | 在 `tegra_channel_mipi_cal` 和 `frame start` 超时分支里 dump CSI 寄存器，判断 clock lane 有没有进 Tegra |
| 2 | 找 Jetson TK1 的 L4T R21.x 驱动包 | 2-3 小时 | 其中有 **glibc 版**的 `libnvmm_camera.so`；走原厂 PCL 架构（Android 10 验证过） |
| 3 | 刷回 MIUI 抓 dmesg | 2-3 小时 | 边际价值低（端口/lane 已穷举），且有变砖风险 |
| 4 | 示波器测 CSI clock lane | 需设备 | 终极硬件确认 |

### 关键判断点（第 1 步）

```
如果 CSI_PHY_STATUS 的 clock lane lock 位 = 0
  → MIPI clock lane 没进 Tegra → 硬件走线 或 settle time 问题

如果 clock lane lock = 1 但 data lane 报错
  → lane 顺序 / settle 参数问题，可修

如果全 lock 但仍无帧
  → VI 侧配置问题，继续查
```

---

## 十一、产物清单

### 内核（WSL: /opt/kbuild/）

| 文件 | SHA256 前缀 | 内容 |
|---|---|---|
| `uImage-camfix13` | `490bc987` | **最新**：DPD + 负载开关 + 完整时序 + 全套日志 |
| `uImage-camfix12` | `d9e60556` | 模式表写入日志 |
| `uImage-camfix10` | `0c59116a` | MCLK / s_stream 日志 |
| `uImage-camfix9` | `d59309ab` | 双名字 regulator 查找 |
| `uImage-camfix8` | `2af8b16d` | PMIC 负载开关 |
| `uImage-camfix7` | `7367fad1` | 原厂 GPIO + 完整时序 |
| `uImage-camfix6` | `5a43c759` | dvdd 支持 |
| `uImage-camfix5` | `1320f4ff` | `gpio_direction_output` |
| `uImage-camfix3` | `670d5e88` | OTP 容错 |
| `uImage-r241` | `69edc7f6` | 基线（与运行版等价） |
| `kernel-original-uImage` | `26c4c15a` | 原版（最终回滚） |

### 设备树（L4T/camwork/）

| 文件 | SHA256 前缀 | 内容 |
|---|---|---|
| `mocha-cam20.dtb` | `d9b005cb` | **当前使用**：csi-port=0, 2 lane, pwdn=222, reset=225 |
| `mocha-cam19.dtb` | `b95d31d3` | csi-port=1 |
| `mocha-cam17.dtb` | `0a2a1710` | 错误的电源映射（勿用） |
| `mocha-cam15.dtb` | `6652a5ce` | pwdn=221（错误的 GPIO） |
| `mocha-cam12.dtb` | `c881f54d` | 最早可用版本 |
| `mocha-v4.dtb` | `e4a5f86f` | 早期无相机节点版本 |

### 参考资料

| 路径 | 内容 |
|---|---|
| `/opt/mocha-oss/Xiaomi_Kernel_OpenSource-mocha-kk-oss/` | **小米官方内核源码**（所有突破的来源） |
| `/opt/stock-dtb/` | MIUI9 原厂 DTB（`tegra124-mocha.dtb`，sha `4a274221`） |
| `/opt/stock-cam/` | 原厂 PCL 用户空间（14 个文件，Android-only） |
| `/opt/mokee-dev/` | MoKee Q device tree（揭示 PCL 架构） |
| `TegraK1_TRM_DP06905001_public_v03p.pdf` | Tegra K1 技术参考手册（查 CSI 寄存器用） |

### 脚本（L4T/）

| 脚本 | 用途 |
|---|---|
| `flash-kernel.sh` / `flash-kernel2.sh` | fastboot + RAM 安装器刷内核 |
| `flash-camfix*.sh` | 各版本快速刷入（SSH） |
| `sweep-csi2.sh` | CSI 端口 × lane 自动枚举 |
| `test-pattern.sh` | 彩条测试 |
| `force-i2c-read.sh` | 绕过驱动强制读 i2c |
| `check-pss*.sh` | 各类诊断脚本 |

---

## 十二、回滚方法

### 恢复到"相机不可用但系统正常"的状态

```bash
# 1. 刷回基线内核
cat /opt/kbuild/uImage-r241 | ssh ubuntu@<IP> 'cat > /tmp/k'
ssh ubuntu@<IP> 'sudo sh -c "mount /dev/mmcblk0p29 /mnt/p29; \
  cp /tmp/k /mnt/p29/l4t/boot/uImage-desktop; sync; reboot"'

# 2. 刷回无相机节点的 DTB
cat L4T/camwork/mocha-v4.dtb | ssh ubuntu@<IP> 'cat > /tmp/d'
ssh ubuntu@<IP> 'sudo sh -c "mount /dev/mmcblk0p29 /mnt/p29; \
  cp /tmp/d /mnt/p29/l4t/boot/mocha-desktop.dtb; sync; reboot"'
```

### 完全恢复原厂

```bash
# 使用 artifacts/kernel-original-uImage（sha 26c4c15a）+ 原厂 DTB
```

### 设备死机（内核不启动）

```
1. 长按电源 30 秒强制关机
2. 电源 + 音量减 → fastboot
3. fastboot boot artifacts/3gb/uboot/mocha-uboot-ramdebug-3gb.img
4. tools/load-ram-nofb-win.py 加载 RAM 安装器
5. ssh -i artifacts/maintenance-key root@172.31.124.2
6. blockdev --setrw /dev/mmcblk0 /dev/mmcblk0p29
7. mount /dev/mmcblk0p29 /mnt/l4t
8. 换内核 → sync → reboot -f
```

---

## 十三、经验教训

1. **官方源码是金矿。** 本篇 6 个突破全部来自 `mocha-kk-oss`。遇到板级问题时，先找官方源码，而不是猜。
2. **"驱动 probe 成功" 不等于 "硬件工作"。** 传感器 probe 成功、i2c 可读写、模式表写入返回 0 —— 但 MIPI 依然没数据。
3. **测试图案是黄金诊断手段。** 一条 `echo 1 > /sys/module/.../test_mode` 就把问题从"传感器"缩小到"MIPI 链路"。
4. **`is_streaming` 卡死陷阱。** 杀进程测采集会导致后续 `s_stream` 永不调用，必须先重启。
5. **不要相信"看起来完整"的假设。** 我一度以为模式表是空的（因为 `ov5693_start[]` 只有一条），实际完整表在另一个数组里，且和原厂逐字节一致。
6. **电源轨的"负载开关"容易被忽略。** PMIC SMPS 只是上游，真正的开关可能是 PMIC 的 GPIO 控制的分立器件。

---

## English Summary

### Achievements

This session moved the OV5693 camera on the Xiaomi Mi Pad 1 (mocha, Tegra K1) from *completely dead i2c* to *only the Tegra CSI PHY link remaining*:

1. **Kernel buildability** — the exact 3.10.108 source tree was identified and a working build/flash/boot cycle established.
2. **Sensor power** — the critical discovery: the camera's 1.8 V and 1.2 V rails are **PMIC GPIO-gated load switches** (`palmas_gpio` 1 and 2), not direct SMPS outputs. Enabling the upstream SMPS alone left the sensor unpowered.
3. **Correct GPIOs** — `CAM2_PWDN` = 222 and `CAM2_RSTN` = PCC1 = 225 (the initial DTB had them swapped, then wrongly "fixed" to `CAM1_PWDN` = 221).
4. **Power sequencing** — PWDN and RSTN must be driven low *before* the rails come up.
5. **GPIO direction** — `gpio_request()` alone is insufficient; `gpio_direction_output()` is required or `gpio_set_value()` silently does nothing.
6. **MCLK** — Tegra124 names the camera clock `mclk2` (not `cam_mclk1`); measured 24 MHz.
7. **CSI DPD** — CSIA/CSIB/CSIE deep power-down must be released.

After all of this the sensor responds on i2c, the full 262-register mode table is written successfully (byte-identical to the vendor PCL table), the media graph and subdev bind, `/dev/video0` exists, and `ov5693_s_stream(1)` is called.

### Remaining blocker

The Tegra VI/CSI receives **no MIPI data at all** — not even from the sensor's internal colour-bar test pattern. All four CSI ports × 2/4 lane combinations were swept with identical results.

Three independent sources (stock MIUI DTB, MoKee Q device tree, both Smoke kernel DTS trees) confirm that this board has **never used the v4l2/VI-graph camera path** — it uses NVIDIA's proprietary PCL path, which performs the MIPI PHY initialisation in ODM code that the mainline VI driver does not replicate.

The extracted PCL userspace is Android/bionic-only and cannot load on Ubuntu.

### Next steps

1. Dump CSI PHY status registers on frame-start timeout to determine whether the MIPI clock lane reaches the Tegra PHY (~40 min, zero risk).
2. If that fails: obtain the Jetson TK1 L4T R21.x driver package, which ships **glibc** PCL userspace libraries.
3. Last resort: re-flash MIUI to capture a working `dmesg` (low marginal value — port and lane count are already ruled out — and carries brick risk).
