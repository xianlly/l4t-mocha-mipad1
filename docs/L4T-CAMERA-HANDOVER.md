# L4T 相机攻坚 —— 交接文档（2026-10-29 深夜）

> **新会话请先读这篇。** 它记录了当前状态、已确认的结论、下一步动作。
> 详细历史见 `L4T-CAMERA-ROOT-CAUSE.md` 和 git log。

---

## 一、一句话现状

**MIPI PHY 校准已经修通**（靠 MIUI 实测寄存器值），CSI/VI 编程确认正常，
v4l2 采集能出 JPEG —— **但图像是固定黑帧**，因为传感器 → MIPI 这一段没有真实数据。

---

## 二、当前设备状态

| 项目 | 值 |
|---|---|
| 系统 | L4T Ubuntu 14.04（p29:/l4t/root）|
| 内核 | `camfix28`（含 CSI 调试输出 `DEBUG 1`）|
| IP | 192.168.29.99 或 .66（DHCP 会变，用扫描脚本）|
| SSH | `ubuntu/ubuntu` |
| U-Boot | p22，`uboot-l4t-3gb.img`(ca277399，读 `/l4t/boot`) |
| 相机节点 | `/dev/video0` = vi-output-0, ov5693 2-0036 |
| 显示/网络/蓝牙/音频 | 全部正常 |

---

## 三、已确认的结论（不要再重新排查）

### ✅ 1. 硬件是好的
MIUI 的 dmesg 有 `tegradc tegradc.0: DSI pad calibration done` —— MIPI_CAL 块可用。

### ✅ 2. 根因是 L4T R24.1 删掉了 T124 的 MIPI 支持
- `mipical/` 只在 `CONFIG_ARCH_TEGRA_21x_SOC` 下编译
- `of_match` 只认 `nvidia,tegra210-mipical`
- `mipi_cal.h` 里 T124 是 `return -ENOSYS` 的 stub

### ✅ 3. 已做过的修复（在 `/opt/kbuild/r241/.../mipi_cal.c`）

```
① Makefile: obj-$(CONFIG_TEGRA_MIPI_CAL) += mipical/
② of_match 加 nvidia,tegra124-mipical + t124_mipi_data
③ 时钟: clk_get_sys("mipi-cal") / ("mipi-cal-fixed") 回退
④ 补 clk72mhz（★ 关键！补上后寄存器才"活"）
⑤ t124 CSI bias: pad_driv_up_ref=0x3, e_vclamp_ref=0x1
⑥ select_lanes(): CSI 不设 SELA
```

**结果：PHY 校准从"永远超时"变成"成功返回"** ✓

### ✅ 4. MIUI 的实测寄存器值（决定性数据）

见 `release/miui-regs/mipi-cal-1.txt`，要点：

```
0x00 = 0x2a000010   CTRL（和我们一致）
0x08 = 0x00317a60   STATUS：bit16 CAL_DONE, bit20 CAL_DONE_CSIA ✓
0x0c = 0x0000001b   STATUS_2
0x14 = 0x00000000   CILA（不设 SELA）
0x58 = 0x00000001   BIAS_PAD_CFG0（E_VCLAMP_REF=1）
0x5c = 0x00000300   BIAS_PAD_CFG1（DRIV_UP_REF=3）
```

---

## 四、当前卡点

```
v4l2 采集 → 得到 1280x720 合法 JPEG（15,027 字节）
移动摄像头后 MD5 完全相同 (38dc9646...)   ← 固定黑帧
→ VI 拿到的是空缓冲区，传感器没有送出 MIPI 数据
```

**CSI 日志显示编程完全正常**（`pp_write`/`cil_write`/`csi_write 0xd0=1` 全对）。

---

## 五、下一步（按顺序）

### 第 1 步：确认传感器是否在输出

**问题**：内核没编 `CONFIG_I2C_CHARDEV` → `/dev/i2c-2` 不存在 → 用户空间读不了传感器。

**解决**（二选一）：

**A. 给 ov5693 驱动加 sysfs 寄存器读取（推荐，10 行）**

```c
// drivers/media/i2c/ov5693.c
static ssize_t ov5693_reg_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    u16 reg = 0x0100;  /* 可通过 echo 设置 */
    u8 val = 0;
    ov5693_read_reg(client, reg, &val);   /* 驱动里已有的函数 */
    return sprintf(buf, "0x%04x = 0x%02x\n", reg, val);
}
static DEVICE_ATTR(reg, 0444, ov5693_reg_show, NULL);
/* probe 里 device_create_file(&client->dev, &dev_attr_reg); */
```

然后：
```bash
cat /sys/bus/i2c/devices/2-0036/reg
```

**B. 编 `CONFIG_I2C_CHARDEV=y`** → 就能用 `i2ctransfer`。

### 第 2 步：读这些寄存器判断状态

| 寄存器 | 含义 | 期望 |
|---|---|---|
| `0x300a` | 芯片 ID 低位 | 0x56（OV5693）|
| `0x3008` | 控制/软复位 | bit7=1 表示 streaming |
| `0x3501/0x3502` | 曝光 | 非 0（模式表设的）|
| `0x3508/0x3509` | 增益 | 非 0 |
| `0x3010-0x3015` | 时序/格式 | 与模式表对比 |

**如果曝光=0 → 模式表没生效**（检查 `ov5693_s_stream` 的写入序列）
**如果读不到 → 传感器掉线**（供电/i2c 时钟）

### 第 3 步：如果传感器正常，查 MIPI 速率

```bash
# CSI 的 lane 速率配置（pp_write 的 val 里）
# 0x2a0301f0 = 控制字，可解码出 lane 数和像素格式
```

---

## 六、文件位置速查

```
/opt/kbuild/r241/SmokeR24.1-kernel-SmokeR24.1-L4T/   内核源码（含全部修复）
   drivers/media/platform/tegra/mipical/mipi_cal.c   ★ 主要修改处
   drivers/media/platform/tegra/csi/csi.c            DEBUG 已设 1
   drivers/media/platform/tegra/camera/channel.c     MIPI_CAL(T124) 分支
   drivers/media/i2c/ov5693.c                        传感器驱动（含调试打印）

/mnt/c/Users/xiany/Desktop/Pisces-Moze-mocha-debian/
   release/artifacts/           可刷内核（camfix27/28）+ 快照
   release/miui-regs/           ★ MIUI 实测寄存器数据
   release/docs/                全部文档
   L4T/*.sh                     全部脚本（刷机/测试/恢复）

/opt/mocha-oss/                 官方 KitKat 源码（参考实现）
/opt/stock-dtb/stock.dts        原厂 DTB
```

## 七、常用命令

```bash
# 编译内核
bash L4T/verify-and-build-clk72.sh          # 出 zImage
# 打包 + 安装
bash L4T/install-test-camfix25.sh           # 含重启+测试

# 测试相机
gst-launch-1.0 -e v4l2src device=/dev/video0 num-buffers=1 \
  ! video/x-raw,width=1280,height=720 ! jpegenc ! filesink location=/tmp/f.jpg

# 看 CSI 日志
dmesg | grep -iE 'csi|pp_write|cil_write|MIPI_CAL'

# 恢复 L4T（设备刷了别的东西后）
bash L4T/full-restore-from-miui.sh
```

## 八、血泪教训

1. **`fastboot boot` 经常失败，要重试 10-20 次**
2. **U-Boot 有两个版本**：`release/artifacts/uboot-l4t-3gb.img`(ca277399，读 `/l4t/boot`) ✓ 用这个；
   `artifacts/3gb/uboot/mocha-uboot-3gb.img`(fc140478，读 `/boot`) ✗ 别用
3. **MIUI 刷机会清空 p29**，需要完整恢复（rootfs + U-Boot + boot 文件）
4. **`pkill make` 会留下孤儿进程卡死编译**，要 `pkill -9 -f make` 再编
5. **`HOSTCFLAGS` 必须作为 make 参数**（不是环境变量），否则 dtc 链接失败
6. **PowerShell 里 `$(...)` 会被吃掉** —— 复杂命令写成 `.sh` 文件再跑
