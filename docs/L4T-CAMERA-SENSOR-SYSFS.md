# L4T 相机：OV5693 寄存器 sysfs 与 CSI 诊断（2026-10-30）

> 接续 `L4T-CAMERA-HANDOVER.md`。本次把「传感器到底有没有在输出」从猜测变成了可读数据，
> 并顺带修通了 T124 的 MIPI 校准，最后把问题收敛到 **CSI 端口 E**。

---

## 一、一句话结论

**OV5693 活着、I2C 可读、`0x0100=0x01` 处于 streaming 模式、曝光/增益/帧长都正确；
T124 MIPI 校准现在能完成（camfix30）；但 CSIA 的 CIL 三个 lane 全是控制错误——
因为这块板子的前置 OV5693 实际接在 CSI E，而设备树一直写的是 `csi-port=0`。**

---

## 二、新增的 sysfs 寄存器接口（本次交付）

代码：`drivers/media/i2c/ov5693.c`，补丁 `release/patches/ov5693-sysfs-regs.patch`。

设备节点下多出 7 个属性：

```
/sys/bus/i2c/devices/2-0036/
  status         一次性汇总 + 结论
  streaming      driver 状态 + 传感器 0x0100
  exposure       0x3500/0x3501/0x3502 -> 行数
  gain           0x350a/0x350b -> 倍数
  frame_length   0x380e/0x380f
  regs           常用寄存器表（带名字）
  reg            通用 16 位寄存器读写窗口
```

用法：

```bash
# 读一个寄存器：先写地址，再读
echo 3501 > /sys/bus/i2c/devices/2-0036/reg
cat /sys/bus/i2c/devices/2-0036/reg
# -> 0x3501 0x7b

# 写一个寄存器：地址 + 值
echo "350a 10" > /sys/bus/i2c/devices/2-0036/reg

# 一句话看状态
cat /sys/bus/i2c/devices/2-0036/status
```

### 2.1 地址修正（交接文档里的几处笔误）

| 项目 | 交接文档写法 | 实际 |
|---|---|---|
| 增益 | `0x3508/0x3509` | **`0x350a/0x350b`**（`0x3508/9` 是短曝光） |
| 曝光 | `0x3501/0x3502` | **`0x3500/0x3501/0x3502`**（20 位，`0x3500` 低 4 位是高 4 位） |
| 芯片 ID | `0x5693` | **`0x5690`**（`0x300a=0x56`, `0x300b=0x90`） |
| 流状态 | `0x3008` bit7 | **`0x0100` bit0**（`0x3008` 是系统控制，不是 stream 位） |

---

## 三、实测数据（camfix29 / camfix30 / camfix31）

### 3.1 空闲时

```
ov5693 2-0036  power=on
I2C read failed (-121)
verdict: IDLE - sensor is powered down or in standby; start a stream and read again
```

`-121 = -EREMOTEIO`：s_stream(0) 之后电源轨/时钟被关掉，传感器 NAK 是正常的。
**不要把它误判成「传感器坏了」**；要读寄存器必须先开流。

### 3.2 开流后（camfix29，`gst-launch` 1280x720）

```
ov5693 2-0036  power=on  driver_stream=1
  chip id    : 0x5690 (OV5693)
  0x0100     : 0x01 (streaming)
  exposure   : 0x07ba0 = 1978.00 lines
  gain       : 0x0010 = 1.00x
  frame len  : 0x07c0 = 1984 lines
verdict: STREAMING - sensor is in output mode; verify MIPI activity on the CSI side
```

`regs` 的关键值：

```
0x0100 stream       0x01
0x3008 sys_ctrl0    0x00
0x300a chip_id_h    0x56
0x300b chip_id_l    0x90
0x3010 pll1_ctrl    0x31
0x3011 pll1_mult    0x21
0x3012 pll1_sel     0x09
0x3013 pll1_div     0x10
0x3500 exp_h        0x00
0x3501 exp_m        0x7b
0x3502 exp_l        0xa0
0x3503 aec_ctrl     0x07      <- 手动 AEC/AGC，所以两次采样曝光/增益不变
0x350a gain_h       0x00
0x350b gain_l       0x10
0x380e frame_len_h  0x07
0x380f frame_len_l  0xc0
0x4800 mipi_ctrl    0x20      <- discontinuous clock, 2 lane
```

**结论：传感器侧完全正常，已经按 262 条模式表配置好并在 streaming。
曝光 1978 行、增益 1.00x、帧长 1984 行，与 `ov5693_mode_tbls.h` 的 2592x1944 模式一致。**

但是抓到的 JPEG 仍然全黑，VI 仍然 `frame start syncpt timeout`。

---

## 四、T124 MIPI 校准：找到并修掉了最后一个错误

### 4.1 根因

`MIPI_CAL_CTRL`（0x00）由 bootloader 留下 `0x2a000000`，而 MIUI 在相机 streaming 时是：

```
0x2a000010 = NOISE_FLT(0xa) | PRESCALE(2) | CLKEN_OVR(1) | AUTOCAL_EN(0)
```

`CLKEN_OVR`（bit4）没置位，T124 的模拟校准块永远不产生 `CAL_DONE`。
之前的代码只 `regmap_update_bits(..., STARTCAL, 0x1)`，**从未写过 CTRL**。

另外，之前有一段「STARTCAL 之后立刻关 clk72mhz」的实验代码，也是错的：
校准块需要 72 MHz 一直跑到 `AUTO_CAL_DONE` 锁存。

### 4.2 修复（camfix30）

`release/patches/t124-mipi-cal-ctrl.patch`：

```c
#define T124_MIPI_CAL_CTRL_VAL 0x2a000010

/* 在 STARTCAL 之前 */
regmap_write(mipi->regmap, MIPI_CAL_CTRL, T124_MIPI_CAL_CTRL_VAL);
regmap_update_bits(mipi->regmap, MIPI_CAL_CTRL, STARTCAL, 0x1);
/* 删掉 STARTCAL 后立即 clk_disable_unprepare(clk72mhz) 的代码 */
```

### 4.3 结果（camfix30，CSIA）

```
mipi_dbg: CTRL before clear = 0x2a000010
mipi_dbg: STATUS before clear = 0x30c17a50
mipi_dbg: STATUS after clear = 0x00007a50
mipi_dbg: CTRL after STARTCAL = 0x2a000010
mipi_dbg: cal done in 12 ms, val=0x17a60
```

**校准从「2 秒超时」变成「6-12 ms 完成」**，`0x17a60` 的 bit16 是 `CAL_DONE`。
这是本次最硬的进展。

### 4.4 一个失败的实验：SELA

camfix31 试着在 CSIA 上置 `SELA`（`CILA_MIPI_CAL_CONFIG` bit21），结果校准又超时：

```
CILA cfg = 0x00200000
Mipi cal timeout, val:7a61, lanes:100000
```

所以 **CSIA 的 SELA 必须保持 0**（与 MIUI streaming 时的 `0x14=0` 一致）。
camfix32 已回退这个实验。

> 注意：camfix30 的 `0x17a60` 只有通用 `CAL_DONE`，没有 MIUI 的 per-lane
> `CAL_DONE_CSIA/CSIB`（bit20/21）。如果 OV5693 根本不在 CSIA 上，这正好解释得通。

---

## 五、CSI CIL 状态：三个 lane 全是控制错误

camfix31 在 `tegra_csi_status()` 里把 CIL/PHY 状态改成 `dev_info`，超时后能看到：

```
TEGRA_CSI_PIXEL_PARSER_STATUS 0x00000000
TEGRA_CSI_CIL_STATUS          0x00000010
TEGRA_CSI_CILX_STATUS         0x00040041
TEGRA_CSI_CIL_PHY_CONTROL     0x0000000a
TEGRA_CSI_CIL_PAD_CONFIG0     0x00000000
TEGRA_CSI_PHY_CIL_COMMAND     0x00000001
```

按 Tegra K1 TRM（26.16.25 / 26.16.26）解码：

| 位 | 含义 |
|---|---|
| `CIL_STATUS` bit4 (`0x10`) | `CILA_CTRL_ERR`：LP 01/10 后直接回 LP11，没进 Escape/Turnaround |
| `CILX_STATUS` bit0 (`0x1`) | `CILA_CLK_LANE_CTRL_ERR`：**时钟 lane 的线序错误** |
| `CILX_STATUS` bit6 (`0x40`) | `CILA_DATA_LANE0_CTRL_ERR` |
| `CILX_STATUS` bit18 (`0x40000`) | `CILA_DATA_LANE1_CTRL_ERR` |

**三个 lane（clock + 2 data）全部报控制错误**，而 `PHY_CIL_COMMAND=1` 说明 CSIA 的
CIL 已经使能。也就是说：**CSIA 上根本没有收到合法的 MIPI LP→HS 序列**。

这不再是「PHY 没校准」的问题（校准已经成功），而是「数据不在 CSIA 上」。

---

## 六、关键推断：OV5693 在 CSI E，不在 CSI A

### 6.1 官方源码的证据

`/opt/mocha-oss/.../arch/arm/mach-tegra/board-ardbeg-sensors.c`：

```c
static int ardbeg_ov5693_power_on(struct ov5693_power_rail *pw)
{
    ...
    /* disable CSIA/B IOs DPD mode to turn on camera for ardbeg */
    tegra_io_dpd_disable(&csie_io);      // <-- 实际解除的是 CSI E
    ...
}
```

注释写的是 CSIA/B，但**实际调用的是 `csie_io`**。错误清理路径里又去 enable CSIA/B，
属于复制粘贴；真正为 OV5693 解除 DPD 的是 **CSI E**。

### 6.2 设备树一直写的是 0

`L4T/camwork/cam20.dts` 两处：

```
ov5693@36/ports/port@0/endpoint { csi-port = <0x00>; ... }
host1x/vi/ports/port@0/endpoint { csi-port = <0x00>; ... }
```

`csi-port=0` 就是 CSIA。端口穷举脚本 `sweep-csi2.sh` 只跑了 0..3，**从没试过 4（CSIE）**。

### 6.3 改成 4 之后

cam21 DTB（`release/artifacts/dtb-cam21.dtb`，sha `210a5ef8…`）把两处改成 `<0x04>`。
用 camfix31 + cam21 启动后，VI 确实创建了 **`vi-output-4`**：

```
vi vi: creating link for channel vi-output-4
vi vi: creating ov5693 2-0036:0 -> vi-output-4:0 link
```

但 `tegra_channel_mipi_cal()` 只映射了 PORT_A..PORT_D：

```
MIPI_CAL(T124): port0=4 numlanes=2 ...
MIPI_CAL: unknown port 4
```

所以端口 4 的流没有校准、也没有使能 CIL（`PHY_CIL_COMMAND=0`）。
**这是 camfix32 要解决的唯一问题。**

### 6.4 camfix32

`release/patches/t124-csi-port-ef.patch`：

```c
case PORT_E:
    lanes = CSIE;
    break;
case PORT_F:
    lanes = CSIF;
    break;
```

camfix32（sha `2bc054dd…`）= camfix31 的 sysfs/CSI 日志 + CTRL 修复 + E/F 映射。
配 cam21 DTB 使用。

---

## 七、当前状态与下一步

### 7.1 已完成

| 项目 | 状态 |
|---|---|
| OV5693 sysfs 寄存器接口 | ✅ 已在设备上验证 |
| 传感器 streaming / 曝光 / 增益 / 帧长 | ✅ 读出并确认正常 |
| T124 MIPI 校准完成 | ✅ camfix30 起 6-12 ms 完成 |
| CIL 状态可观测 | ✅ camfix31 起超时打印 |
| CSIE 端口假设 | ✅ 官方源码 + `vi-output-4` 证据 |
| camfix32 + cam21 | ✅ 编译完成，**待实机测试** |

### 7.2 待测

设备当前不在主机网络里（主机已切到 `192.168.183.141/24`，设备最后一次是
`10.210.131.66`，两个网段都扫不到 22 端口）。需要让设备重新上线（同一 Wi-Fi 或 USB）。

设备上线后直接跑：

```bash
bash L4T/install-test-camfix32.sh
```

它会自动找 IP、刷 camfix32、重启、用 `/dev/video4` 抓彩条和正常帧，并把
`tp-camfix32.jpg` / `norm-camfix32.jpg` 取回 `L4T/`。

**判据：**

* 彩条 JPEG 不再是 30 KB 全黑、能看到彩条 → CSIE 假设成立，收工。
* 仍然 syncpt timeout → 看 `TEGRA_CSI_CIL_*` 是否变成 CSIE 的寄存器；
  若 CSIE 也报控制错误，再查 lane 映射 / settle / 传感器 MIPI 输出。

### 7.3 回滚

设备上 `/mnt/p29/l4t/boot/mocha-desktop.dtb.bak-cam20` 是改端口前的 cam20 DTB。
恢复：

```bash
sudo sh -c 'mount /dev/mmcblk0p29 /mnt/p29; \
  cp /mnt/p29/l4t/boot/mocha-desktop.dtb.bak-cam20 /mnt/p29/l4t/boot/mocha-desktop.dtb; \
  sync; reboot'
```

内核可回滚到 `kernel-camfix30-uImage`（校准可用、CSIA 黑帧）或更早的 camfix28。

---

## 八、产物

| 文件 | sha256（前 16 位） | 说明 |
|---|---|---|
| `release/artifacts/kernel-camfix29-uImage` | `1047b1d4e02eb028` | 仅 sysfs，用于确认传感器状态 |
| `release/artifacts/kernel-camfix30-uImage` | `14bd59c95c103996` | **CTRL 修复，校准 6-12 ms 完成** |
| `release/artifacts/kernel-camfix31-uImage` | `8cc145a6fe17d962` | SELA 实验（会超时）+ CIL 日志 |
| `release/artifacts/kernel-camfix32-uImage` | `2bc054ddcea1fdd6` | CTRL + E/F 映射；CSIE 校准仍超时 |
| `release/artifacts/kernel-camfix33-uImage` | `86d9aa6f2a861078` | **所有 CIL SEL=0，CSIE 校准 6 ms 完成** |
| `release/artifacts/kernel-camfix34-uImage` | `45975fee93975faf` | **CIL BYPASS_LP_SEQ，时钟 lane 错误消失** |
| `release/artifacts/dtb-cam21.dtb` | `210a5ef80ee23ee1` | `csi-port=<4>`（CSIE） |
| `release/artifacts/dtb-port1.dtb` | `b95d31d3c9f6f9cf` | `csi-port=<1>`（CSIB） |
| `release/patches/ov5693-sysfs-regs.patch` | — | sysfs 接口 |
| `release/patches/t124-mipi-cal-ctrl.patch` | — | CTRL=0x2a000010 + 保持 clk72 |
| `release/patches/t124-csi-port-ef.patch` | — | PORT_E/F 校准映射 |

---

## 九、下午的后续实验（2026-10-30 13:00）

### 9.1 camfix33：所有 CIL SEL 清零后，CSIE 校准也能完成

camfix32 在 CSIE 上仍然超时（`val:7a61`），因为 `select_lanes()` 给 CSIE 置了 `SELE`。
把 **所有 CILx 的 SEL 位都清 0** 后（camfix33）：

```
MIPI_CAL(T124): port0=4 ... lanes=0x1000000 -> calibrating
mipi_dbg: cal done in 6 ms, val=0x17a60
```

**结论：SELx 位本身会让 T124 校准挂死；CSIA 和 CSIE 都必须在 SEL=0 下校准。**
camfix33 sha `86d9aa6f…`。

但是 CSIE 的 CIL/PHY 寄存器读出来全是 0：

```
TEGRA_CSI_CIL_STATUS       0x00000000
TEGRA_CSI_CILX_STATUS      0x00000000
TEGRA_CSI_CIL_PHY_CONTROL  0x00000000
TEGRA_CSI_PHY_CIL_COMMAND  0x00000000
```

而 CSIA/CSIB 的同一组寄存器是有值的（`0x10/0x41/0x0a/0x1`）。
说明 **第三个 CSI block（E/F，属于 VI.1）没有被真正上电/复位/时钟化**，
这与根因报告里的「只有一个 VI 实例」一致。camfix33 只修了校准，没修 VI.1。

### 9.2 camfix34：CIL BYPASS_LP_SEQ，时钟 lane 错误消失

`s_stream(1)` 先让传感器开流，之后 CIL 才使能，CIL 会错过
LP11→LP01→LP00 序列。把 `CILx_CONTROL0`（`CIL_PHY_CONTROL`）的 bit6
`BYPASS_LP_SEQ` 置 1，即 `0x0A -> 0x4A`（camfix34）：

```
TEGRA_CSI_CIL_PHY_CONTROL 0x0000004a
TEGRA_CSI_CILX_STATUS     0x00000040     (原来是 0x41)
```

**`CILA_CLK_LANE_CTRL_ERR`（bit0）消失了**，只剩
`CILA_DATA_LANE0_CTRL_ERR`（bit6）。这是目前最接近成功的一步。

camfix34 sha `45975fee…`。在 CSIB（port 1）上验证，仍然 syncpt timeout，
但时钟 lane 已经干净，问题收敛到 **data lane 0 的 LP 序列 / lane 映射 / settle**。

### 9.3 端口实测汇总（camfix33/34）

| port | 校准 | CIL block | 结果 |
|---|---|---|---|
| 0 (CSIA) | ✅ 6-12 ms | 有值，`CILX=0x41` | 时钟+数据 lane 控制错误 |
| 1 (CSIB) | ✅ 12-18 ms | 有值，`CILX=0x41` | 同上；camfix34 后 `CILX=0x40` |
| 4 (CSIE) | ✅ 6 ms（camfix33）| 全 0 | 第三个 CSI block 没起来（VI.1） |

### 9.4 下一步建议

1. **data lane 0 的 settle / 极性**：`CILx_CONTROL0` 低 6 位是 `THS_SETTLE`，
   当前 0xA；bit6 是 `BYPASS_LP_SEQ`。可以先试 `THS_SETTLE` 0x0..0xF，
   或检查 DTB 的 `data-lanes` / lane polarity。
2. **VI.1 / 第三个 CSI block**：如果确认 OV5693 在 CSIE，需要让 VI.1 上电，
   或者把第三个 CSI block 的 power/reset 接上；这是比调 settle 更大的工程。
3. **端口 0 复核**：用 camfix34 在 CSIA 再跑一次，确认 `CILX` 是否也变成 `0x40`；
   如果 CSIA/CSIB 都只剩 data lane 0 错误，说明是共性的 lane 映射问题。

### 9.5 当前设备状态

- 内核：camfix34（#39，`45975fee…`）
- DTB：`cam-port1`（`csi-port=1`，sha `b95d31d3…`）
- IP：`192.168.183.66` / `.99`（DHCP 会变）
- 采集：仍为全黑 30,054 字节 JPEG，`frame start syncpt timeout`

---

## 十、English Summary

The OV5693 is alive and streaming: chip ID `0x5690`, `0x0100=0x01`, exposure
1978 lines, gain 1.00x, frame length 1984. The new sysfs interface under
`/sys/bus/i2c/devices/2-0036/` exposes `status`, `streaming`, `exposure`, `gain`,
`frame_length`, `regs` and a generic `reg` window.

The T124 MIPI calibration now completes in 6–12 ms after programming
`MIPI_CAL_CTRL=0x2a000010` (CLKEN_OVR=1, as MIUI does) and keeping `clk72mhz`
running until `AUTO_CAL_DONE`. Setting `SELA` on CSIA makes it hang, so SELA
stays 0.

The CIL status proves CSIA receives no valid MIPI LP→HS sequence: clock lane and
both data lanes report control errors. The official board file releases
`csie_io` DPD for the OV5693, and a `csi-port=4` DT creates `vi-output-4` — so
the front camera is on **CSI E**, not CSI A. `camfix32` adds the missing PORT_E/F
calibration mapping; it is built but still needs a real-device test with the
cam21 DTB and `/dev/video4`.

---

## 十一、原理图确认：CSIE + 单 lane（2026-10-30 晚）

用户提供了 `xiaomi_mi_pad1_schematic.pdf`（27 页），第 18 页是 SoC 的
CAM CSI/DSI 引脚，第 25 页是前后摄像头座子。结论是决定性的：

### 11.1 前置 OV5693 = CSI E，只有 1 条 data lane

```
第 18 页：
  CSI_E_CLK  -> MIPI_CSI1_CLK_P/M
  CSI_E_D0   -> MIPI_CSI1_LANE0_P/M      <-- 只有 lane0

第 25 页 J1502（FRONT CAMERA）：
  MIPI_CSI1_CLK_P/M
  MIPI_CSI1_LANE0_P/M
  CAM2_MCLK / CAM2_PWDN / CAM2_RST_N / CAM_I2C
```

后置 IMX179 才是：

```
CSI_A/B -> MIPI_CSI0_LANE0..3           (4 lane)
```

所以之前的 DTB 错了两个地方：

| 项目 | 之前 | 正确 |
|---|---|---|
| csi-port | 0（CSIA） | **4（CSIE）** |
| bus-width / num-lanes | 2 | **1** |
| data-lanes | `<1 2>` | **`<1>`** |

### 11.2 按原理图改完后的实测

DTB `dtb-port4-1lane.dtb`（sha `d2004ae0…`）+ camfix34：

```
ENABLE_STREAM enter: ... port0=4 numlanes=1
MIPI_CAL(T124): port0=4 numlanes=1 ...
mipi_dbg: cal done in 6 ms, val=0x17a60
```

**端口和 lane 数都对了，校准也过了。** 但是：

```
TEGRA_CSI_CIL_STATUS       0x00000000
TEGRA_CSI_CILX_STATUS      0x00000000
TEGRA_CSI_CIL_PHY_CONTROL  0x00000000
TEGRA_CSI_PHY_CIL_COMMAND  0x00000000
```

CSIE 的 CIL/PHY 寄存器仍然全 0。开流时 `cile` 时钟是 `refcnt=1, state=1`，
所以不是时钟没开，而是**第三个 CSI block（E/F）没有真正复位/上电**。

### 11.3 camfix35：给第三个 block 打复位脉冲

在 `tegra_csi_start_streaming()` 里，配置 CIL 之前先：

```c
cil_write(port, TEGRA_CSI_CIL_SW_SENSOR_RESET, 0x1);
csi_write(csi, TEGRA_CSI_CSI_SW_STATUS_RESET, 0x1, port_num >> 1);
usleep_range(10, 20);
cil_write(port, TEGRA_CSI_CIL_SW_SENSOR_RESET, 0x0);
csi_write(csi, TEGRA_CSI_CSI_SW_STATUS_RESET, 0x0, port_num >> 1);
```

camfix35 sha `decf4382…`，已编译，设备离线未测。

### 11.4 MIUI CSI/VI dump 工具

`release/artifacts/csidump`（静态 ARM，sha `90e914d0…`）：

```bash
csidump          # 0x54080000..0x540a0000  VI + CSI A/B/C/D/E/F
csidump mipi     # 0x700e3000..0x700e4000  MIPI_CAL
```

在 MIUI 前置相机预览时抓 `csi-front.txt` / `mipi-front.txt`，可以确认：

- MIUI 实际用哪个 CSI block
- CIL 的 THS_SETTLE / BYPASS_LP_SEQ
- `PHY_CIL_COMMAND` 是否使能
- 第三个 block 在 MIUI 下是否有值

### 11.5 成功率

有了原理图，成功率从 30–40% 上调到 **50–60%**：

- 端口/lane 已确定并改对
- 校准已过
- 只剩第三个 CSI block 的复位/上电
- 如果 camfix35 不行，MIUI 的 CSI dump 能直接给出 ground truth
