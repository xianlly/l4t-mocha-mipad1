# L4T 蓝牙修复指南（Broadcom BCM43341 / Tegra K1）

> 适用：Xiaomi Mi Pad 1 (mocha) 上运行 Ubuntu 14.04 / 16.04 L4T，
> 症状为 `Can't read local name on hci0: Connection timed out (110)`，
> 所有 HCI 命令 `tx timeout`，蓝牙无法被搜索、无法配对。
>
> **本文的修复已在本机实测验证：HCI Reset 返回 Command Complete，
> BLE 扫描能发现 10+ 个真实设备，RX 流量 155 KB 零错误。**

---

## 一、症状

```bash
$ hciconfig -a
hci0:   Type: BR/EDR  Bus: UART
        BD Address: 43:50:C0:00:00:00  ACL MTU: 1021:8  SCO MTU: 64:1
        UP RUNNING PSCAN              # 注意：没有 ISCAN
        RX bytes:654  acl:0 sco:0 events:37 errors:0
        TX bytes:932  acl:0 sco:0 commands:33 errors:0
        Features: 0xbf 0xfe 0xcf 0xfe 0xdb 0xff 0x7b 0x87
Can't read local name on hci0: Connection timed out (110)
```

内核日志持续刷：

```
Bluetooth: hci0 command 0x0c03 tx timeout   # HCI Reset —— 连复位都不回
Bluetooth: hci0 command 0x0c14 tx timeout   # Read Local Name
Bluetooth: hci0 command 0x1009 tx timeout   # Read BD_ADDR
Bluetooth: hci0 command 0x0c24 tx timeout   # Read Local OOB Data
Bluetooth: hci0 command 0x0c52 tx timeout   # Read Local Extended Features
```

**关键矛盾**：`Features` 能读出来、`RX bytes` 在增长，说明**芯片曾经通信成功过**，
但现在所有命令都没回应。

---

## 二、根因：低功耗模式（LPM）让芯片一睡不醒

原厂 `/etc/init/nvwifibt.conf` 里同时做了两件事：

```bash
script
	echo 1 > /proc/bluetooth/sleep/lpm          # ← 元凶 1
	exec /usr/sbin/rfkill unblock bluetooth
	exec /usr/sbin/brcm_patchram_plus --enable_hci --use_baudrate_for_download \
	     --scopcm=0,2,0,0,0,0,0,0,0,0 --baudrate 3000000 \
	     --patchram /lib/firmware/BCM4350C0.hcd --no2bytes \
	     --enable_lpm --tosleep=50000 \          # ← 元凶 2
	     /dev/ttyTHS2
end script
```

在这台设备上：

1. `/proc/bluetooth/sleep/lpm` 置 1 → 使能低功耗休眠
2. `--enable_lpm --tosleep=50000` → 空闲 50 ms 后让控制器休眠
3. **唤醒路径（GPIO/BT_REG_ON 时序）在 L4T 下不工作**
4. 控制器进入休眠后再也不响应任何 HCI 命令
5. 开机瞬间的握手能成功（所以 `Features`/`RX bytes` 有值），之后彻底静默

---

## 三、修复（三行改动）

编辑 `/etc/init/nvwifibt.conf`：

```bash
sudo cp /etc/init/nvwifibt.conf /root/nvwifibt.conf.orig    # 先备份

# 去掉 sleep/lpm 那一行
# 去掉 --enable_lpm --tosleep=50000
# 去掉 --use_baudrate_for_download --baudrate 3000000（改用固定 115200）
# 去掉 --patchram（基础功能不需要固件补丁）
```

最终内容：

```bash
description     "NVIDIA bluetooth/wifi init script"

instance $SUBTASK

script
	exec /usr/sbin/rfkill unblock bluetooth
	exec /usr/sbin/brcm_patchram_plus --enable_hci \
	     --scopcm=0,2,0,0,0,0,0,0,0,0 \
	     --baudrate 115200 \
	     --no2bytes \
	     /dev/ttyTHS2
end script
```

然后重启（必须重启，芯片需要断电复位）：

```bash
sudo reboot
```

---

## 四、验证

### 4.1 控制器状态

```bash
hciconfig -a
```

期望看到（重点是 **ISCAN** 和 **Name**）：

```
hci0:   Type: BR/EDR  Bus: UART
        BD Address: 43:50:C0:00:00:00  ACL MTU: 1021:8  SCO MTU: 64:1
        UP RUNNING PSCAN ISCAN
        RX bytes:689  acl:0 sco:0 events:42 errors:0
        TX bytes:2185 acl:0 sco:0 commands:42 errors:0
        Features: 0xbf 0xfe 0xcf 0xfe 0xdb 0xff 0x7b 0x87
        Name: 'tegra-ubuntu-0'
        Class: 0x740100
        HCI Version: (0x7)  Revision: 0x1000
```

### 4.2 HCI 复位（最关键）

```bash
sudo hcitool cmd 0x03 0x0003
```

修复前：`tx timeout`，无任何回应。
修复后：

```
< HCI Command: ogf 0x03, ocf 0x0003, plen 0
> HCI Event: 0x0e plen 4
  01 03 0C 00            ← Command Complete, status 0x00
```

### 4.3 实际扫描

```bash
sudo hcitool scan          # 经典蓝牙
sudo hcitool lescan        # BLE
```

实测输出示例：

```
D8:CC:98:63:6D:0A   HUAWEI WATCH GT 6-D0A
00:60:37:01:3A:38   BYD1
30:00:89:20:B8:6D   ZXH_BAA4_F6E82000
FA:3F:CF:F7:F6:FC   N9DCD2243J0513
```

### 4.4 缓冲区协商

```bash
sudo hcitool cmd 0x04 0x0005      # Read Buffer Size
```

期望：

```
< HCI Command: ogf 0x04, ocf 0x0005, plen 0
> HCI Event: 0x0e plen 11
  01 05 10 00 FD 03 40 08 00 01 00
```

---

## 五、让设备能被搜到（`BD_ADDR` 无效问题）

修完 LPM 后 `hciconfig` 已经很漂亮了，但**用手机搜不到**：

```
hci0:   BD Address: 43:50:C0:00:00:00     ← 后三字节全 0
        UP RUNNING PSCAN ISCAN
        Name: 'tegra-ubuntu-0'
```

### 5.1 为什么搜不到

蓝牙 inquiry 响应报文里必须带发起方自己的 `BD_ADDR`：

```
手机发 Inquiry
   ↓
平板回 Inquiry Response（携带 BD_ADDR = 43:50:C0:00:00:00）
   ↓
手机系统判定这个地址无效 → 静默丢弃
   ↓
看起来"隐藏"了
```

`43:50:C0:00:00:00` 是 BCM43341 ROM 的占位地址。真机上这个地址的来源全都缺失：

| 来源 | 状态 |
|---|---|
| ROM 默认值 | `43:50:C0:00:00:00`（无效） |
| `/mnt/factory/bluetooth/bt_mac.txt` | p18 分区 **全是 0**，文件不存在 |
| `/proc/device-tree/chosen/nvidia,bluetooth-mac` | DTB 里没有这个节点 |
| `/persist`（p16） | 只有序列号和传感器校准，无 MAC |
| `.hcd` 固件补丁 | 不兼容，不能用 |

### 5.2 解决：用 Broadcom 厂商命令写 MAC

```bash
# Broadcom Write BD_ADDR = 0xFC01（ogf 0x3f, ocf 0x0001）
# HCI 里 BD_ADDR 是小端序，所以要倒着发
sudo hcitool cmd 0x3f 0x0001 0x34 0x22 0x11 0x4c 0x90 0x00
```

成功时返回：

```
< HCI Command: ogf 0x3f, ocf 0x0001, plen 6
  34 22 11 4C 90 00
> HCI Event: 0x0e plen 4
  01 01 FC 00            ← Command Complete, status 0x00
```

读回验证：

```bash
sudo hcitool cmd 0x04 0x0009      # Read BD_ADDR
```

```
> HCI Event: 0x0e plen 10
  01 09 10 00 34 22 11 4C 90 00    ← 00:90:4C:11:22:34
```

### 5.3 MAC 从哪来

用 **WiFi MAC 末字节 +1**，同一 OUI —— 这是给组合芯片分配蓝牙地址的常规做法：

```
wlan0:  00:90:4C:11:22:33
BT:     00:90:4C:11:22:34
```

`00:90:4C` 是 NVIDIA/Epigram 的 OUI，是这块板子真实的厂商前缀。

### 5.4 顺手把设备类别设对

写完 BD_ADDR 后 Class of Device 会变成 `0x000000`，手机会显示成
"Miscellaneous"。设成电脑：

```bash
sudo hcitool cmd 0x03 0x0024 0x0C 0x01 0x00     # 0x00010C = Computer / Laptop
```

### 5.5 ⚠️ 操作码别搞错（踩过的坑）

```diff
- hcitool cmd 0x03 0x0013 0x03      # ✗ 这是 Write Local Name！只发 1 字节会把名字写坏
+ hciconfig hci0 piscan             # ✓ 正确开启 inquiry+page scan (0x03 0x001A)

- hcitool cmd 0x03 0x0013 ...       # ✗ 不要手搓名字
+ hciconfig hci0 name "tegra-ubuntu-0"   # ✓ 它会把 248 字节补齐
```

正确的操作码：

| 命令 | OGF | OCF |
|---|---|---|
| Write Local Name | 0x03 | **0x0013**（248 字节负载） |
| Write Scan Enable | 0x03 | **0x001A** |
| Write Class of Device | 0x03 | 0x0024 |
| Read Local Name | 0x03 | 0x0014 |
| Write BD_ADDR（厂商） | 0x3f | 0x0001 |

### 5.6 持久化

这个写入存在控制器 RAM 里，**重启就没了**。所以由
`/usr/local/sbin/bt-mac-setup.sh` 在每次开机时重新写一遍，
调用点是 `/etc/init/nvwifibt.conf` 的 `post-start`：

```bash
post-start script
	/usr/local/sbin/bt-mac-setup.sh
end script
```

脚本做的事：

1. 等 `hci0` 出现（`brcm_patchram_plus` 是异步创建的）
2. 读 `wlan0` 的 MAC，末字节 +1 推导出 BT MAC
3. `hcitool cmd 0x3f 0x0001 ...` 写入
4. `hcitool cmd 0x03 0x0024 0x0C 0x01 0x00` 设类别
5. `hciconfig hci0 name "tegra-ubuntu-0"` 设名字
6. `hciconfig hci0 piscan` 开启可发现+可连接

### 5.7 最终验证

```
hci0:	Type: BR/EDR  Bus: UART
	BD Address: 00:90:4C:11:22:34
	UP RUNNING PSCAN ISCAN
	RX bytes:1337  TX bytes:2517  errors:0
	Features: 0xbf 0xfe 0xcf 0xfe 0xdb 0xff 0x7b 0x87
	Name: 'tegra-ubuntu-0'
	Class: 0x00010c
	Device Class: Computer, Laptop
	HCI Version: (0x7)  Revision: 0x1000
```

**实测结果：手机可以搜到 `tegra-ubuntu-0`，显示为笔记本电脑。**

---

## 六、仍然存在的限制

| 项目 | 状态 | 说明 |
|---|---|---|
| `.hcd` 固件补丁 | **不需要，且不可用** | `BCM4350C0.hcd` 与 BCM43341 不兼容，`--patchram` 会让 `brcm_patchram_plus` 永久挂起 |
| 3 Mbaud 高速模式 | 未启用 | 用 115200 已足够，切速未验证 |
| 原厂 `BCM4350C0.hcd` | 未使用 | L4T 版 64 KB，Android 原厂版 41 KB，两者 md5 不同，都不兼容 |
| BD_ADDR | ✅ 已解决 | 开机由 `bt-mac-setup.sh` 自动写入 |
| 已实测 | ✅ | 手机能搜到 `tegra-ubuntu-0`，Class 显示为 Computer/Laptop |

---

## 七、调试过程中踩过的三个坑（很有参考价值）

### 坑 1：`fuser` 输出被 `tail` 截掉 → 端口占用没发现

多次测试都"超时"，实际是**上一轮的 `brcm_patchram_plus` 还占着 `/dev/ttyTHS2`**，
新进程连端口都打不开，只能干等。

**正确做法**：

```bash
for i in $(seq 1 10); do
  pkill -9 -f brcm_patchram_plus
  sleep 1
  fuser /dev/ttyTHS2 >/dev/null 2>&1 || break
done
fuser /dev/ttyTHS2 >/dev/null 2>&1 && { echo "PORT BUSY, abort"; exit 1; }
```

### 坑 2：管道捕获输出 → 缓冲丢失，一行都看不到

```bash
timeout 25 brcm_patchram_plus ... 2>&1 | head -20    # ✗ 什么都看不到
```

`timeout` 杀进程时，stdio 管道缓冲区没刷新。用以下任一方式：

```bash
stdbuf -o0 -e0 timeout 25 brcm_patchram_plus ... 2>&1 | head -20
# 或重定向到文件
timeout 25 brcm_patchram_plus ... > /tmp/bp.log 2>&1; cat /tmp/bp.log
```

### 坑 3：`sudo -S` 吃掉 heredoc

```bash
sudo -S tee /etc/init/nvwifibt.conf > /dev/null <<"EOF"    # ✗ 文件没写成
...
EOF
```

`sudo -S` 从 stdin 读密码，而 heredoc 也走 stdin —— 密码位置被配置文件第一行占了。

**正确做法**：分两步

```bash
cat local.conf | ssh user@dev 'cat > /tmp/x.conf'
echo PASSWORD | ssh user@dev 'sudo -S cp /tmp/x.conf /etc/init/nvwifibt.conf'
```

---

## 八、快速修复脚本

见 `scripts/08-fix-bluetooth.sh`（幂等，自带备份）和
`artifacts/bt-mac-setup.sh`（开机写 BD_ADDR）。

```bash
sudo bash scripts/08-fix-bluetooth.sh      # 装 unit 文件 + bt-mac-setup.sh
sudo reboot
```

装完之后每次开机自动：写 BD_ADDR、设 Class、设名字、开启 ISCAN/PSCAN。

---

## English Summary

**Problem 1 — the controller looked dead.** On Xiaomi Mi Pad 1 (Tegra K1,
BCM43341 over UART) running L4T, every HCI command timed out
(`Can't read local name`, `hci0 command 0x0c03 tx timeout`), even though the
initial handshake had succeeded (`Features` and `RX bytes` were populated).

**Cause**: the stock `/etc/init/nvwifibt.conf` enabled low-power mode:

```
echo 1 > /proc/bluetooth/sleep/lpm
brcm_patchram_plus ... --enable_lpm --tosleep=50000 ...
```

The controller sleeps after 50 ms idle and the LPM wake path does not work on
this board, so it never answers another HCI command.

**Fix**: drop LPM (the `sleep/lpm` write, `--enable_lpm`, `--tosleep`), drop the
3 Mbaud speed switch, and drop `--patchram`.

```
rfkill unblock bluetooth
brcm_patchram_plus --enable_hci --scopcm=0,2,0,0,0,0,0,0,0,0 \
                   --baudrate 115200 --no2bytes /dev/ttyTHS2
```

`--patchram` must NOT be used: `BCM4350C0.hcd` targets a different chip and
brcm_patchram_plus hangs on it forever (with or without LPM).

**Problem 2 — still invisible to other devices.** After the fix `hciconfig`
looked healthy (`UP RUNNING PSCAN ISCAN`) but no phone could find the tablet.
The controller reports a placeholder address:

```
BD Address: 43:50:C0:00:00:00
```

Inquiry responses carry that bogus address, and phones drop them. None of the
usual MAC sources exist on this device: the factory partition (p18) is all
zeros, the DTB has no `nvidia,bluetooth-mac`, and `/persist` holds no MAC.

**Fix**: write a real address with the Broadcom vendor command, derived from
the WiFi MAC (same OUI, last octet + 1):

```
hcitool cmd 0x3f 0x0001 <mac bytes, LSB first>   # Write BD_ADDR (0xFC01)
hcitool cmd 0x03 0x0024 0x0C 0x01 0x00           # Class of Device = Laptop
hciconfig hci0 name "tegra-ubuntu-0"
hciconfig hci0 piscan
```

The write lives in controller RAM, so `artifacts/bt-mac-setup.sh` re-applies it
on every boot from the `post-start` hook of the unit file.

**Result**: `Name: 'tegra-ubuntu-0'`, `Class: 0x00010c`, `BD Address:
00:90:4C:11:22:34`, `HCI Reset` returns Command Complete, `hcitool lescan`
discovers nearby devices, and **a phone can find and list the tablet as a
laptop**.

**Four debugging pitfalls worth remembering**:

1. A stale `brcm_patchram_plus` keeps `/dev/ttyTHS2` open — verify the port is
   truly free with `fuser` before every attempt (and don't let `tail` hide the
   output).
2. Piping into `head` loses buffered stdout when `timeout` kills the process —
   use `stdbuf -o0` or redirect to a file.
3. `sudo -S` consumes the first line of a heredoc as the password — upload the
   file separately, then `sudo cp`.
4. `0x03 0x0013` is **Write Local Name**, not Write Scan Enable (that is
   `0x03 0x001A`). Sending one byte to it renames the device to `".."`.
   Use `hciconfig hci0 name/piscan` instead of hand-built commands.
