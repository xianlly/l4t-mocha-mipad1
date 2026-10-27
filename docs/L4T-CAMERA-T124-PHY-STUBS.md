# 2026-10-27 突破：找到两层空壳（相机攻克的最终定位）

> 接续 `L4T-CAMERA-BREAKTHROUGH.md`。本篇记录了把"MIPI 收不到数据"追到**确切代码位置**的过程。
> 结论：v4l2 相机路径在 Tegra124 上缺失 MIPI D-PHY 校准实现（两层空壳），这是根本原因。

---

## 一、本日净进展

| 项目 | 结果 |
|---|---|
| 彩条测试确认 | ✅ 连传感器内部彩条都收不到 → 排除传感器/模拟部分 |
| CSI 端口 × lane 穷举 | ✅ 8 组全失败（0/1/2/3 × 2/4 lane）→ 排除端口和 lane 数 |
| CSI PHY 诊断日志 | ✅ 加入 channel.c 并定位到真正执行的分支 |
| **发现第 1 层空壳** | ✅ `channel.c` 的 T124 版 `tegra_channel_mipi_cal()` = `return 0` |
| **发现第 2 层空壳** | ✅ `mipi_cal.h` 的 T124 版 `tegra_mipi_calibration()` = `return -ENOSYS` |
| 第 1 层已修复 | ✅ 实现了 T124 的 port → lane mask 映射并调用校准 |
| 第 2 层未修复 | ❌ 需要实现 T124 的 CSI PHY 校准本体 |

---

## 二、决定性证据链

### 2.1 彩条测试（排除传感器侧）

```bash
echo 1 > /sys/module/ov5693/parameters/test_mode
# 采集 → 仍然全零 + syncpt timeout
```

传感器内部彩条发生器也到不了 VI → **问题 100% 在接收侧**。

### 2.2 8 组枚举（排除端口/lane）

| csi-port | lanes | 非零字节 | syncpt 超时 |
|---|---|---|---|
| 0 | 2 | 1 | 3 |
| 0 | 4 | 1 | 3 |
| 1 | 2 | 1 | 4 |
| 1 | 4 | 1 | 3 |
| 2 | 2 | 1 | 3 |
| 2 | 4 | 1 | 4 |
| 3 | 2 | 1 | 4 |
| 3 | 4 | 1 | 4 |

### 2.3 `tegra_channel_enable_stream()` 的参数（全部正确）

```
vi vi: ENABLE_STREAM enter: pg_mode=0 valid_ports=1 port0=0 numlanes=2
vi vi: ENABLE_STREAM set_stream(true) -> 0
```

`pg_mode=0`（非图案发生器）、`valid_ports=1`、`numlanes=2`、`set_stream` 返回 0 —— 一切符合预期。

### 2.4 第一层空壳

```c
// drivers/media/platform/tegra/camera/channel.c
#if defined(CONFIG_ARCH_TEGRA_21x_SOC)
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	... 完整校准逻辑（选择 lane mask、写 CSI_PHY_CIL_COMMAND、调用 tegra_mipi_calibration）
}
#else
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	return 0;          /* ← Tegra124 上什么都不做 */
}
#endif
```

### 2.5 第二层空壳

```c
// drivers/media/platform/tegra/mipical/mipi_cal.h 第 39 行
int tegra_mipi_calibration(int lanes) { return -ENOSYS; }

// 真正实现在 mipi_cal.c:463，但依赖 mipi_cal_t21x.h —— T210 专用
```

**结论：L4T R24.1 的相机 MIPI 校准基础设施是 T210（Jetson TX1）专用代码，T124 上两层都是空壳。**

这解释了为什么这块板子只能用 PCL（NvOdm 自己做 PHY 初始化）。

---

## 三、第 1 层已修复的代码

```c
#else
/*
 * mocha port: on Tegra124 this used to be a stub that did nothing, so the
 * MIPI D-PHY was never calibrated and the CSI receiver could never lock onto
 * the sensor's clock lane.
 */
static int tegra_channel_mipi_cal(struct tegra_channel *chan, char is_bypass)
{
	unsigned int lanes = 0;

	dev_info(chan->vi->dev,
		 "MIPI_CAL(T124): port0=%u numlanes=%u total_ports=%u valid_ports=%u bypass=%d\n",
		 chan->port[0], chan->numlanes, chan->total_ports,
		 chan->valid_ports, is_bypass);

	switch (chan->port[0]) {
	case PORT_A: lanes = CSIA; break;
	case PORT_B: lanes = CSIB; break;
	case PORT_C: lanes = CSIC; break;
	case PORT_D: lanes = CSID; break;
	default:
		dev_err(chan->vi->dev, "MIPI_CAL: unknown port %u\n", chan->port[0]);
		return -EINVAL;
	}

	dev_info(chan->vi->dev, "MIPI_CAL(T124): lanes=0x%x -> calibrating\n", lanes);
	return tegra_mipi_calibration(lanes);
}
#endif
```

**运行结果**：

```
vi vi: MIPI_CAL(T124): port0=0 numlanes=2 total_ports=1 valid_ports=1 bypass=0
vi vi: MIPI_CAL(T124): lanes=0x100000 -> calibrating
```

`lanes = 0x100000` = CSIA（bit 20）✓ 映射正确。但因为第 2 层仍是空壳，实际校准并未发生。

---

## 四、第 2 层要做什么（未完成）

### 参考代码

```
drivers/video/tegra/dc/mipi_cal.c       ← T124 的 MIPI 校准实现（DSI 用）
drivers/video/tegra/dc/dsi.c:2329
    tegra_mipi_calibration(DSIA | DSIB)  ← 在显示屏上正常工作
```

**MIPI_CAL 寄存器块（APB_MISC，0x700e3000）是 CSI 和 DSI 共享的**，所以可以在 DSI 实现基础上扩展 CSI lane。

### 需要实现的内容

| 项目 | 说明 |
|---|---|
| lane mask 扩展 | CSIA..CSIF（DSI 只有 DA/DB） |
| 寄存器偏移 | 每个 lane 组的 `MIPI_CAL_CONFIG` 位置 |
| settle time 计算 | 按 MIPI 数据速率算 `THS_SETTLE` / `CLK_SETTLE` |
| bias pad 使能 | `MIPI_BIAS_PAD_CFG0/1` |
| 校准触发与轮询 | 启动校准、等待 done 位 |

### 工作量

| 项目 | 评估 |
|---|---|
| 代码量 | 200-400 行 |
| 时间 | 3-6 小时（含查 TRM） |
| 成功率 | 35-45% |
| 风险 | 只改内核，可完全回滚 |

**参考文档**：`TegraK1_TRM_DP06905001_public_v03p.pdf` 的 MIPI_CAL 章节

---

## 五、摄像头攻克的最终状态（封存）

### 已证明正常（不可再怀疑）

```
✅ 传感器上电：SMPS + PMIC GPIO 负载开关(vdd_cam_1v8/1v2) + PWDN(222) + RSTN(225)
✅ 主时钟：24.000 MHz 实测
✅ i2c：读/写/262 条模式表全部成功
✅ 传感器配置：与官方 PCL 表逐字节一致
✅ 媒体图：subdev 绑定 + link 创建
✅ /dev/video0 = "vi-output-0, ov5693 2-0036"
✅ s_stream 调用链完整，返回 0
✅ CSI IO DPD 已解除（CSIA/CSIB/CSIE）
✅ MIPI cal 入口已修通（第 1 层）
```

### 唯一缺失

```
❌ Tegra124 的 CSI MIPI D-PHY 校准实现（第 2 层）
   → PHY 未初始化 → clock lane 锁不上 → 无数据
```

### 封存结论

**摄像头不是"配错了"，而是"这块芯片的 v4l2 相机路径在 L4T 24.x 里从未实现完整"。**

三种可能出路（按性价比）：

| 路线 | 内容 | 时间 | 成功率 |
|---|---|---|---|
| A | 实现 T124 CSI PHY 校准（参照 DSI 版本 + TRM） | 3-6 h | 35-45% |
| B | L4T R21.x 内核 + glibc 版 PCL 用户空间（原厂架构） | 5-8 h | 40-50% |
| C | 放弃（硬件可能确实只能走 PCL） | 0 | — |

---

## English Summary

The camera investigation is now definitively localised: **the Tegra124 CSI MIPI D-PHY calibration is missing from L4T R24.x**, where it exists only for T210.

Two consecutive stubs were found:

1. `channel.c` — `tegra_channel_mipi_cal()` on T124 is `return 0;`
2. `mipi_cal.h` — `tegra_mipi_calibration()` on T124 is `return -ENOSYS;`

Layer 1 has been implemented (port → lane-mask mapping, verified emitting `lanes=0x100000` for CSIA). Layer 2 — the actual PHY calibration — still requires writing a T124 CSI implementation, using `drivers/video/tegra/dc/mipi_cal.c` (the working DSI version) as a template and the Tegra K1 TRM for register offsets.

Everything on the sensor side is proven correct: power (including the PMIC GPIO load switches), 24 MHz MCLK, full i2c access with the vendor-identical 262-register mode table, media graph, subdev binding, `s_stream`, and CSI IO DPD release. Even the sensor's own colour-bar test pattern cannot reach the VI, confirming the fault is entirely on the receive side.

The camera work is archived and frozen at this point. The device remains fully functional as a Linux tablet (4 cores, 2976 MB RAM, display, touch, WiFi, Bluetooth, audio, GPU), with only extra debug logging and camera DT nodes remaining from the experiments.
