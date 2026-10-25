# L4T 摄像头调查报告（Tegra K1 / Xiaomi Mi Pad 1）

> 本文记录对 `mocha`（Xiaomi Mi Pad 1，Tegra K1，BCM43341）在 L4T 14.04 上
> 摄像头不可用问题的完整调查：**已经做到什么、卡在哪里、为什么卡住**。
>
> 结论先行：**设备树层面已经全部打通（`/dev/video0`+`/dev/video1` 已创建），
> 但最终卡在一个 NVIDIA 闭源用户空间守护进程（`camera_daemon`）缺失，
> 且无法用现有组件替代。**

---

## 一、设备上到底是什么摄像头

从 **Android 侧**读出来的（这是最权威的来源）：

```bash
# /mnt/android/etc/nvcamera.conf
version=1
camera0=/dev/imx179,back,0,mono      // 后置 = IMX179
camera1=/dev/ov5693,front,270,mono   // 前置 = OV5693
```

两条关键信息：

| 摄像头 | 传感器 | i2c |
|---|---|---|
| 后置 | **IMX179**（Sony 8MP） | bus 2, addr `0x10` |
| 前置 | **OV5693**（OmniVision 5MP） | bus 2, addr `0x36` |

地址是从设备树里交叉验证出来的（见第三章）。

---

## 二、内核侧的摄像头栈：**完整的，而且能初始化**

### 2.1 硬件模块全部就绪

```
tegra_camera_platform tegra-camera-platform.38: camera_platform_driver probe  ✓
misc tegra_camera_ctrl: tegra_camera_isomgr_register camera's max_iso_bw 800000 ✓
isp isp.0: initialized   ✓
isp isp.1: initialized   ✓
vi vi: vi_probe: ++ / vi initialized  ✓
```

### 2.2 传感器驱动是**编译进内核**的

```bash
$ grep -iE 'ardbeg_(ov|imx)' /proc/kallsyms
ardbeg_ov5693_power_on / ardbeg_ov5693_power_off / ardbeg_ov5693_front_power_off
ardbeg_ov7695_power_on / ardbeg_imx179_power_on
```

dmesg 也确认：

```
[IMX135] sensor driver loading
[IMX179] sensor driver loading
[imx132] sensor driver loading
[ar0261] sensor driver loading
[ov10823] sensor driver loading
```

**注意 `ardbeg_` 前缀** —— Ardbeg 是 NVIDIA Shield Tablet 的代号，这个 L4T
DTB 就是从 Shield Tablet 的 BSP 来的。

### 2.3 内核认识哪些摄像头 DT 属性（从内核镜像里挖字符串）

```bash
# 从 uImage 里解出内核再 grep
$ strings vmlinux.bin | grep -Fx num-channels        # ← 命中！
$ strings vmlinux.bin | grep -Fx drivernode           # 0
$ strings vmlinux.bin | grep -Fx proc-device-tree     # 0
$ strings vmlinux.bin | grep -Fx num_csi_lanes        # 0
$ strings vmlinux.bin | grep -Fx v4l2_sensor          # 0
```

**只有 `num-channels`，没有现代 L4T 24.x 的 `drivernode` / `proc-device-tree`
/ `num_csi_lanes`。** 说明这个内核是 **L4T 21.x 时代的 PCL 架构**。

同时确认内核认识这些：

```
remote-endpoint ✓   csi-port ✓   mclk ✓   vana/vdig ✓   reset-gpios ✓
nvidia,imx179 ✓     nvidia,imx135 ✓   nvidia,imx132 ✓
nvidia,ov5693 ✗     ← 前置没有 V4L2 驱动，只有 PCL
```

---

## 三、设备树：两个关键修复（已验证有效）

### 3.1 `camera-pcl` 被显式禁用

原始 DTB：

```dts
camera-pcl {
    compatible = "nvidia,tegra124-camera", "simple-bus";
    configuration = <0xaa55aa55>;
    status = "disabled";          // ← 元凶 1
    modules { ... imx179 / ov5693 / ad5823 ... };
    profiles { imx179@2_0010 / ad5823@2_000c / ov5693@2_0036 };
};
```

改成 `status = "okay"` 之后，PCL 框架立刻活了：

```
pcl-generic pcl-generic: virtual_chip_add     (×3，对应 3 个 profile)
```

### 3.2 ⭐ `tegra-camera-platform` 缺 `num-channels`（**核心发现**）

原始 DTB 里这个节点几乎是空的：

```dts
tegra-camera-platform {
    compatible = "nvidia, tegra-camera-platform";
};
```

后果（内核日志）：

```
vi vi: Failed to find num of channels, set TPG mode
vi vi: Set TPG mode to 2
```

**VI 找不到通道数 → 退回到 TPG（测试图案发生器）模式 → 不创建 `/dev/video*`。**

修复：

```dts
tegra-camera-platform {
    compatible = "nvidia, tegra-camera-platform";
    num-channels = <2>;          /* 2 个摄像头 */
};
```

再重启，**TPG 错误消失，`/dev/video0` 和 `/dev/video1` 被创建**：

```
crw-rw---- 1 root video 81, 0 /dev/video0
crw-rw---- 1 root video 81, 1 /dev/video1
crw-rw---- 1 root video 10,52 /dev/camera.pcl
```

> 这个 `num-channels` 也可以同时加到 `vi` 节点上（无副作用，作为保险）。

### 3.3 i2c 总线编号（踩过的坑）

传感器地址 `2-0010` / `2-0036` 里的 **`2-` 是 Linux 的 i2c 总线号**，
而总线号由 DT `aliases` 决定：

```
i2c0 -> /i2c@7000c000       ← 我一开始把传感器放这里，设备名变成 0-0010（错）
i2c1 -> /i2c@7000c400
i2c2 -> /i2c@7000c500       ← 正确！
i2c3 -> /i2c@7000c700
i2c4 -> /i2c@7000d000
i2c5 -> /i2c@7000d100
```

验证方法：

```bash
for a in /proc/device-tree/aliases/i2c*; do
  echo "$(basename $a) -> $(tr -d '\0' < $a)"
done
```

放错总线的报错是：

```
imx179 0-0010: imx179_regulator_get vana ERR: -19      ← -19 = ENODEV
```

放对总线后变成：

```
imx179 2-0010: Dev node doesn't have vana supply props
```

### 3.4 供电：不需要猜，厂方留了映射表

DT 里的 regulator 节点带有 **NVIDIA 的 regulator-consumer 表**，
直接写明了"哪个 supply 给哪个设备"：

```dts
ldo4 {                                  /* regulator-name = "avdd-cam", 2.7V */
    consumers {
        c5 { regulator-consumer-supply = "vana";  regulator-consumer-device = "2-0010"; };
        c6 { regulator-consumer-supply = "avdd";  regulator-consumer-device = "2-0010"; };
        ...
    };
};
```

完整映射（用脚本从 DTS 里解析出来的）：

```
设备 2-0010 (IMX179):  vdig, vdig_lv, vana, avdd, vif, vdd_af1
设备 2-0036 (OV5693):  dvdd, vdig, avdd_ov5693, dovdd, vif, vana
```

对应 regulator：

| supply | regulator | phandle |
|---|---|---|
| `vdig` | `vdd-lcd-io` | `0x1d` |
| `vana` / `avdd` | `avdd-cam` (ldo4, 2.7V) | 需自行分配 |
| `vif` | `vdd-cam-af` (1.8V) | 需自行分配 |

**这些 regulator 节点原本没有 phandle**（因为没人用 phandle 引用它们）。
补充方法：给节点加 `phandle` + `linux,phandle`（用高于现有最大值的新号，避免冲突），
然后在 sensor 节点里写 `vana-supply = <&...>`。

补完之后驱动的供电报错消失：

```
[IMX179]: probing sensor.
[IMX179]: end of probing sensor.        ← 干净
```

### 3.5 ⚠️ 但是：PCL 和 V4L2 节点会抢同一个 i2c 地址

加了 `imx179@10` 之后：

```
i2c i2c-2: Failed to register i2c client pcl_IMX179 at 0x10 (-16)   ← EBUSY
pcl-generic: camera_new_device cannot allocate client: pcl_IMX179 bus 2, 10
```

**`-16` = EBUSY：PCL 框架要自己创建 `pcl_IMX179` 客户端，地址被我们的 DT 节点占了。**

这说明 **PCL 路径是自给自足的**（它从 `camera-pcl/profiles` 读总线/地址，
自己建 i2c client），**不需要**额外的 i2c 设备节点。

所以这两条路互斥：

| 路径 | 需要 | 结果 |
|---|---|---|
| PCL（`/dev/camera.pcl`） | 只保留 `camera-pcl` 节点 | ✓ PCL 正常 |
| V4L2（`/dev/video*` + subdev） | 额外 i2c 节点 + media graph | ✓ 驱动 probe，但 PCL 失效 |

---

## 四、真正的拦路虎：`camera_daemon` 缺失（闭源）

### 4.1 决定性证据

```bash
$ sudo -u ubuntu GST_DEBUG=3 gst-launch-1.0 -v nvcamerasrc num-buffers=1 ! fakesink
ERROR nvcamerasrc gstnvcamerasrc.cpp:2344:gst_nvcamera_socket_connect:<nvcamerasrc0>
      Connecting to camera_daemon failed
ERROR: Pipeline doesn't want to pause.
```

`nvcamerasrc` 只是个 **socket 客户端**，真正的活儿在守护进程里。
把它里面的字符串挖出来，架构一目了然：

```
send_request_to_camera_daemon
register_buffer_to_camera_daemon
Socket read error. Camera Daemon stopped functioning.....
CameraDaemon_REQ GET NUM SENSORS
CameraDaemon_REQ CREATE SESSION
CameraDaemon_REQ SET SENSOR MODE
```

而系统里**完全没有**这个守护进程：

```bash
$ for b in camera_daemon nvcamera_daemon nvcamera-daemon nvcamd; do command -v $b; done
（全部没有输出）
$ ls /tmp/*camera* /var/run/*camera*          # 无 socket
$ sudo find / -xdev -iname '*camera*daemon*'  # 无任何文件
```

### 4.2 尝试用 Jetson TK1 的 R21.5 包替代（失败）

手上有 `Tegra124_Linux_R21.5.0_armhf.tbz2`（Tegra124 = Tegra K1，同平台）。
里面：

```
Linux_for_Tegra/nv_tegra/nv_sample_apps/nvgstapps.tbz2
    usr/lib/arm-linux-gnueabihf/gstreamer-1.0/libgstnvcamera.so   ← 有插件！
```

**这个插件不需要 daemon**（grep `camera_daemon` = 0），它直接调用
`NvCameraCore_*` API（PCL 时代架构，正合我们的内核）：

```
NvCameraCore_Open / Close / SetSensorMode / FrameCaptureRequest / GetStaticProperties
```

但装上后加载失败：

```
Failed to load plugin 'libgstnvcamera.so':
  libnvmm_camera_v3.so: undefined symbol: NvOsCancelThroughputHint
```

### 4.3 为什么替代不了：69 个旧符号

`libnvmm_camera_v3.so` 需要 69 个符号，我们的 2019 版 NVIDIA 库没有：

| 类别 | 例子 | 能否 shim |
|---|---|---|
| `NvOs*` | `NvOsCancelThroughputHint`, `NvOsSendThroughputHint` | ✅ 可以（性能提示，空实现无害） |
| **`NvRm*`** | `NvRmOpen`, `NvRmStreamInit`, `NvRmStreamBegin/End/Flush`, `NvRmSurfaceInit*`, `NvRmMemHandleAlloc` | ❌ **真实功能，无法伪造** |
| `cu*` / `gl*` / `egl*` | CUDA / OpenGL | ✅ 系统已有 |

（R21.5 的 `libnvos.so` 确实有 `NvOsCancelThroughputHint`，但 R24 的 `libnvos.so`
把它删了 —— 所以这不是"库太旧"，而是 **API 代际差异**。）

### 4.4 试过的其它路线

| 尝试 | 结果 |
|---|---|
| 只替换 `libgstnvcamera.so` + `libnvmm_camera_v3.so` | ❌ 缺 NvOs 符号 |
| 用 `LD_LIBRARY_PATH` 加载 R21.5 全套旧库 | ❌ **把其它插件也搞坏**（`libnvddk_vic.so: undefined symbol: NvRmSyncIsFd` 等），因为旧 `libnvrm.so` 覆盖了全局 |
| `patchelf --set-rpath` 精准隔离 | ❌ Ubuntu 14.04 源里**没有** `patchelf` |
| 写符号 shim（`LD_PRELOAD`） | ❌ 69 个符号里有大量 `NvRmStream*`/`NvRmSurface*` 真实功能 |
| 整体降级到 R21.5 驱动栈 | ⚠️ 会破坏显示/GPU —— 现在这套 2019 版驱动正是让这块板子能跑起来的前提 |

### 4.5 结论

```
内核：L4T 21.x 风格（PCL）
用户空间：L4T 24.x 风格（daemon 客户端）
中间的 camera_daemon：缺失（闭源，无法自行实现）
```

**要让摄像头工作，需要一个与内核同代的、包含 `camera_daemon` 的完整用户空间。**
在这个 rootfs 上无法通过改设备树或替换单个库达成。

---

## 五、如果以后要继续

| 路线 | 说明 |
|---|---|
| **找 R24.1 的 `camera_daemon`** | 必须是 armhf、与 `3.10.108` 内核匹配的版本；NVIDIA 闭源，只能从完整 BSP 里拿 |
| **找带 camera 的 ardbeg/mocha L4T 镜像** | 同代完整用户空间，整体替换风险高 |
| **用 Android 侧验证硬件** | Android 下摄像头正常，说明硬件与内核驱动都没问题 |

## 六、当前状态（诚实记录）

| 层面 | 状态 |
|---|---|
| 硬件 | ✅ 正常（Android 下可用） |
| 内核驱动 | ✅ IMX179 驱动 probe 成功 |
| ISP / VI | ✅ 初始化成功，无 TPG 回退 |
| 设备树 | ✅ 已改好（`camera-pcl` okay + `num-channels = <2>`） |
| `/dev/video0`、`/dev/video1`、`/dev/camera.pcl` | ✅ 已存在 |
| **实际采集** | ❌ 缺 `camera_daemon` |

**`/dev/video*` 存在但不可用于采集** —— 直接对它们做 v4l2 操作会
（因为 `vi-output-255` 的通道号未被正确关联）导致内核崩溃，**不要随便碰**。

---

## English Summary

**Sensors**: read from the Android side (`/mnt/android/etc/nvcamera.conf`):
rear = **IMX179** (i2c-2 addr 0x10), front = **OV5693** (i2c-2 addr 0x36).

**Kernel side is complete.** The kernel is L4T 21.x-era: it only knows the
`num-channels` DT property (no `drivernode` / `proc-device-tree` / `v4l2_sensor`
/ `num_csi_lanes`), and it has the PCL framework (`camera-pcl`, `pcl-generic`)
plus built-in sensor drivers (`ardbeg_imx179_power_on`, `ardbeg_ov5693_*`).
ISP 0/1 and VI all initialise successfully.

**Two DT fixes, both verified:**

1. `camera-pcl { status = "disabled" }` → `"okay"` — brings the PCL framework up
   (`pcl-generic: virtual_chip_add` ×3).
2. **`tegra-camera-platform { num-channels = <2>; }`** — this is the key fix.
   Without it VI logs `Failed to find num of channels, set TPG mode` and creates
   no video nodes. With it the TPG fallback disappears and `/dev/video0` /
   `/dev/video1` / `/dev/camera.pcl` appear.

**Gotchas documented:** the `2-` in `2-0010` is the Linux i2c bus number, which
follows the DT aliases (`i2c2 -> /i2c@7000c500`); the sensor power supplies can
be read straight out of the NVIDIA regulator-consumer table
(`regulator-consumer-device = "2-0010"`); and the PCL framework creates its own
i2c clients, so adding V4L2 i2c nodes makes PCL fail with `-16` (EBUSY) — the two
paths are mutually exclusive.

**The blocker is userspace.** `nvcamerasrc` (v1.2.0, R24-era) is only a socket
client:

```
ERROR gstnvcamerasrc.cpp:2344: Connecting to camera_daemon failed
```

`camera_daemon` is a closed-source NVIDIA component and is **not installed**
anywhere on the system. The R21.5 (Jetson TK1) plugin *does* avoid the daemon
(it calls `NvCameraCore_*` directly), but its `libnvmm_camera_v3.so` needs 69
symbols absent from the 2019-era driver stack — including real APIs such as
`NvRmOpen`, `NvRmStreamInit`, `NvRmSurfaceInit*`, which cannot be stubbed.
`LD_LIBRARY_PATH` with the full R21.5 lib set breaks unrelated plugins, and
`patchelf` is not available in 14.04 to isolate an rpath.

**Conclusion:** the camera cannot be fixed on this rootfs by DT or library
surgery; it needs a matched-generation userspace containing `camera_daemon`.
