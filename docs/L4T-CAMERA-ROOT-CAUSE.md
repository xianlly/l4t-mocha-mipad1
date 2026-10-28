# L4T 相机：根因确认报告（2026-10-28）

> 通过抓取 MIUI9（Android 4.4.4）运行时的完整系统数据，**最终确认了相机在 L4T 上不可用的全部根因**。
> 本篇是相机攻坚的最终技术结论。

---

## 一、决定性证据（全部来自 MIUI 实机数据）

### 1.1 Android 有 `/dev/mipi-cal`，我们没有

```
MIUI /dev 列表:
  crw-rw---- media camera  10, 52 camera.pcl
  crw-rw---- media camera  10, 23 focuser          ← AD5823 对焦
  crw-rw---- media camera  10, 24 imx179           ← 后置传感器
  crw-rw---- media camera  10, 58 mipi-cal         ← ★★★ MIPI 校准设备
  crw-rw---- media camera 248,  1 nvhost-ctrl-isp
  crw-rw---- media camera 239,  1 nvhost-ctrl-vi
```

**L4T 上没有 `/dev/mipi-cal`** → 说明 MIPI 校准驱动从未 probe。

### 1.2 Android 有 `vi.0` 和 `vi.1` 两个 VI 实例

```
<6>[2.511389] vi vi.0: vi_probe: ++
<6>[2.512318] vi vi.1: vi_probe: ++
```

**L4T 只有一个**（`vi vi: vi_probe: ++`）。

Tegra124 有两个 VI brick：
- VI.0 → CSIA / CSIB
- VI.1 → CSIC / CSID / CSIE / CSIF

原厂 DTB 里有 `regulator-consumer-device = "vi.1"` ✓ 说明第二个 VI 是设计中的。

### 1.3 `mipi-cal` 和 `mipi-cal-fixed` 是时钟

```
<4>[2.147227]    mipi-cal-fixed
<4>[2.147237]    mipi-cal
```

官方 KitKat 源码 `drivers/clk/tegra/clk-tegra114.c:1869`：

```c
tegra_clk_register_periph_gate("mipi-cal", "clk_m", 0, clk_base, ...)
```

### 1.4 Android 的 cmdline 与 L4T 不同

```
MIUI:
  syspart=system
  gpt_sector=4095
  console=none
  mem=2006M@2048M  mem=1024M@4096M
  androidboot.selinux=permissive

L4T（我们的）:
  gpt_sector=41983
  console=ttyS0,115200n8 console=tty0
  root=/dev/mmcblk0p29 rootsubdir=/l4t/root
```

### 1.5 Android 没有任何 v4l2 设备

```
/sys/class/video4linux/  → 空
```

**证实：mocha 的相机栈完全是 PCL，没有 v4l2 路径。**

---

## 二、根因（两条，都在 NVIDIA 侧）

### 根因 1：L4T R24.1 删掉了 T124 的 MIPI 校准实现

| | Android (KitKat / R21.x) | L4T R24.1 |
|---|---|---|
| 实现位置 | `drivers/video/tegra/dc/mipi_cal.c`（**252 行库**）| `drivers/media/platform/tegra/mipical/`（T210 专用）|
| of_match | 无（是库，被调用）| `nvidia,tegra210-mipical` ✗ |
| 时钟名 | `mipi-cal` + `mipi-cal-fixed` | `uart_mipi_cal` ✗ |
| 头文件 | 无 | `mipi_cal_t21x.h`（T210 专用）|
| /dev/mipi-cal | ✅ 存在 | ❌ 不存在 |

**我们的 DTB 是 `compatible = "nvidia,tegra124-mipical"` → 驱动不匹配 → 从不 probe → CSI PHY 从未校准。**

### 根因 2：只有一个 VI 实例

```c
// channel.c
#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
   /* 完整逻辑 */
#else
static int tegra_channel_mipi_cal(...) { return 0; }   /* T124 空壳 */
#endif
```

**即使 mipi_cal 库存在，T124 分支也只是 `return 0`。**

---

## 三、修复方案（工作量已明确）

### 步骤 1：移植 252 行 T124 MIPI 校准库

```
源：/opt/mocha-oss/.../drivers/video/tegra/dc/mipi_cal.c   （252 行）
目标：/opt/kbuild/r241/.../drivers/media/platform/tegra/mipical/

需要：
  · 用它的 tegra_mipi_calibration() 替换 T210 版本
  · 处理时钟：mipi-cal / mipi-cal-fixed（tegra12_clocks.c 已注册）
  · 支持 CSIA..CSIF 六个 lane 组
  · 在 probe 里注册 /dev/mipi-cal（misc device）
```

### 步骤 2：启用第二个 VI 实例

```
在 DT 里为 VI.1 添加节点（或让 vi.c 创建两个实例）
参考原厂 DTB 的 vi.1 regulator consumer
```

### 步骤 3：把 channel.c 的 T124 分支接上

```
把 #else 分支的 return 0 换成调用 tegra_mipi_calibration(lanes)
（我此前已实现 port→lane mask 映射，只需接上真实校准）
```

**预计：4-5 小时，成功率 60-70%。**

---

## 四、已抓取并归档的数据

### `L4T/miui-data/`（第一次抓取）

| 文件 | 内容 |
|---|---|
| `cmdline.txt` | Android 真实 cmdline |
| `devices.txt` | 设备节点（含 `/dev/mipi-cal`）|
| `getprop.txt` | 全部系统属性 |
| `dmesg-before.txt` / `dmesg-after.txt` | 开机 + 相机启动日志 |
| `logcat-all.txt` | 完整 logcat |
| `kernel-config.gz` | **Android 内核配置** |
| `camera-files.txt` | 相机库/配置清单 |

### `L4T/miui-data2/`（第二次抓取，拍照后）

| 文件 | 内容 |
|---|---|
| `dmesg-full.txt` | 1419 行，含 `vi.0` / `vi.1` probe |
| `logcat-all.txt` / `logcat-bufs.txt` | 全量日志 |
| `devices.txt` | 设备节点 |
| `partitions.txt` / `byname.txt` | 分区表（与我们相同：APP/CAC/UDA）|
| `nvcamera.conf.txt` | 相机配置 |
| `libs-list.txt` / `hw-list.txt` | 相机库清单 |
| `build.prop.txt` / `modules.txt` | 系统信息 |
| `camera-props.txt` / `v4l2-sysfs.txt` | **v4l2 为空 → 证实 PCL-only** |

### 源码与产物

| 路径 | 内容 |
|---|---|
| `/opt/mocha-oss/` | **官方 KitKat 源码**（含 252 行 `dc/mipi_cal.c`）|
| `/opt/stock-dtb/` | MIUI9 原厂 DTB（`stock.dts`）|
| `/opt/stock-cam/` | 原厂 PCL 用户空间（bionic，Ubuntu 用不了）|
| `/opt/kbuild/r241/` | R24.1 源码 + 全部编译产物 |
| `/opt/android-boot/` | Android boot.img 解出的 kernel/ramdisk/dtb |

---

## 五、恢复 L4T 的方法（重要！）

### 设备当前状态：MIUI 在跑

**刷回 L4T 需要三步**：

```
1. 把 U-Boot 装回 p22（LNX）
   fastboot flash 或 RAM 安装器（storage.sh 支持 LNX=22）
   镜像：artifacts/3gb/uboot/mocha-uboot-3gb.img

2. 恢复 rootfs（p29）
   挂载 p29 → 解压 root-1404-v2.tar.gz
   注意：要先分区/格式化 p29（MIUI 的 GPT 里 p29=UDA 仍存在，56 GB）

3. 恢复 /l4t/boot 三件套
   kernel-camfix16-uImage   4772ed4a
   dtb-cam20.dtb            d9b005cb
   initramfs-mocha-data.cpio.gz  c79697cf
```

### 关键文件清单

| 文件 | 大小 | SHA256 |
|---|---|---|
| `root-1404-v2.tar.gz` | 1.62 GB | `f94ed26cdf39774647b258f6e20bc1c251a680a8e141f328115ec797a9a8e25f` |
| `kernel-camfix16-uImage` | 7.4 MB | `4772ed4ae05dcd75ff1569285d90747adbc61d7aae4ee0a8dc25a7d65edb183b` |
| `dtb-cam20.dtb` | 142 KB | `d9b005cbf6de918cc05eda66106dfd16f478d12587f1ba1c455f1a21b8b6fed9` |
| `initramfs-mocha-data.cpio.gz` | 1.0 MB | `c79697cfd93afb346266504ecc66ae0756c7b54c96bec911f8ded03856571966` |
| `mocha-uboot-3gb.img` | 556 KB | （见 SHA256SUMS）|

---

## 六、English Summary

MIUI9 (Android 4.4.4) running on the device provided the decisive evidence:

1. **`/dev/mipi-cal` exists on Android and not on L4T** — the T124 MIPI calibration driver probes on Android but never on our kernel.
2. **Two VI instances (`vi.0`, `vi.1`) probe on Android; only one on L4T.**
3. **`mipi-cal` and `mipi-cal-fixed` are clocks**, registered by `clk-tegra114.c` in the KitKat kernel.
4. **Android has zero v4l2 devices**, confirming mocha's camera stack is PCL-only.

Root causes:

* **Cause 1 — L4T R24.1 removed the T124 MIPI calibration.** The KitKat kernel implements it as a **252-line library** in `drivers/video/tegra/dc/mipi_cal.c` using the `mipi-cal` / `mipi-cal-fixed` clocks. R24.1 replaced it with a T210-only driver (`nvidia,tegra210-mipical`, clock `uart_mipi_cal`, `mipi_cal_t21x.h`), which never binds to a `nvidia,tegra124-mipical` node.
* **Cause 2 — the T124 branch of `tegra_channel_mipi_cal()` in R24.1 is a stub returning 0.**

Fix plan: port the 252-line T124 library, register `/dev/mipi-cal`, enable the second VI instance, and wire the T124 branch of `tegra_channel_mipi_cal()` to the real calibration. Estimated 4-5 hours, 60-70% success probability.

All captured data, source trees, and boot artefacts are archived; the L4T restore procedure is documented above.
