# Camera snapshot v1 — OV5693 real frames

> 冻结时间：2026-10-30 22:49 CST
> 对应 git tag：`camera-ov5693-v1`
> 对应 commit：`30b33f0`（本文件提交后打 tag）

## 状态

**OV5693 已经能出真实帧**，数据链路完整：

- 1280x720 JPEG：526 / 546 KB（测试图 / 正常图，MD5 不同）
- RAW10：1,843,200 字节，10-bit 数据有效
- PC debayer：能看到真实场景（白色物体、圆形物体）

## 冻结的产物

| 文件 | sha256 |
|---|---|
| `artifacts/kernel-camfix38-uImage` | `f14cf806eaf626aaab3643642fc15547fca939622d7bb08b3553a04852a46544` |
| `artifacts/dtb-port4-1lane.dtb` | `d2004ae01dab350edf5b549f053a7a7c87d136a6082e843cde30a9e1ee93dae5` |

## 冻结的源码

| 文件 | sha256 |
|---|---|
| `camera/src/ov5693.c` | `f756c3ab9d5c3833a32511a36a99f4eb0ddd9c66e20d2218dc2ab0222a42fc40` |
| `camera/src/mipi_cal.c` | `78a291d32e7498198e43699055de0cc10a57f34758e1dbb3494096ceed89bef2` |
| `camera/src/csi.c` | `2f2e18579ed546d949c3eb984063b6857eabe8b34b690247b42ae85ca510994c` |
| `camera/src/channel.c` | `ac03a368a3987a0df07fee03fe774772d0fd406ef7d0b1cec10ba7a6f2af3e0b` |
| `camera/dts/cam-port4-1lane.dts` | `af0dcf178a9773e69eaa47d064c55d474ee41b2cc9abafd6c186c1a081b2cc13` |

## 复现

见 [`docs/L4T-CAMERA-OV5693-REPRODUCE.md`](../docs/L4T-CAMERA-OV5693-REPRODUCE.md)。

## 已知限制（v1 之后继续优化）

1. v4l2 只暴露 Bayer，JPEG 假色（需要 debayer/ISP）。
2. RG10 buffer stride 与 VI 写入宽度不一致，右侧黑边。
3. syncpt timeout 日志仍在，但帧已到达。

## 回滚

- 内核：`artifacts/kernel-camfix16-uImage`
- DTB：`artifacts/dtb-cam20.dtb`
