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

## 五、遗留问题

| 项目 | 状态 | 说明 |
|---|---|---|
| `BD Address` | `43:50:C0:00:00:00` | ROM 默认值。可加 `--bd_addr` 写入，但当前不影响扫描/发现 |
| `.hcd` 固件补丁 | **不需要** | 去掉 `--patchram` 后蓝牙完全正常 |
| 3 Mbaud 高速模式 | 未启用 | 用 115200 已足够；切速在去掉 LPM 后是否安全未验证 |
| 原厂 `BCM4350C0.hcd` | 未使用 | L4T 版 64 KB，Android 原厂版 41 KB，两者 md5 不同 |

---

## 六、调试过程中踩过的三个坑（很有参考价值）

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

## 七、快速修复脚本

见 `scripts/08-fix-bluetooth.sh`（幂等，自带备份）。

```bash
sudo bash scripts/08-fix-bluetooth.sh
sudo reboot
```

---

## English Summary

**Problem**: On Xiaomi Mi Pad 1 (Tegra K1, BCM43341) running L4T, every HCI
command times out (`Can't read local name`, `hci0 command 0x0c03 tx timeout`),
even though the initial handshake succeeds (`Features` and `RX bytes` are
populated) and `UP RUNNING PSCAN` is shown without `ISCAN`.

**Root cause**: The stock `/etc/init/nvwifibt.conf` enables low-power mode:

```
echo 1 > /proc/bluetooth/sleep/lpm
brcm_patchram_plus ... --enable_lpm --tosleep=50000 ...
```

The controller goes to sleep after 50 ms of inactivity, and the LPM wake path
does not work on this board, so it never answers another HCI command.

**Fix**: Remove LPM (`--enable_lpm`, `--tosleep`, and the `sleep/lpm` write),
drop the 3 Mbaud speed switch, and drop `--patchram` (not needed):

```bash
rfkill unblock bluetooth
brcm_patchram_plus --enable_hci --scopcm=0,2,0,0,0,0,0,0,0,0 \
                   --baudrate 115200 --no2bytes /dev/ttyTHS2
```

Reboot, then verify with `hcitool cmd 0x03 0x0003` (should return
`HCI Event: 0x0e`), `hciconfig -a` (should show `ISCAN` and `Name`), and
`hcitool lescan` (should list real nearby devices).

**Result**: HCI Reset returns Command Complete, `Name: 'tegra-ubuntu-0'` is
readable, and BLE scanning discovers 10+ devices with 155 KB of RX traffic
and zero errors.

**Three debugging pitfalls worth remembering**:

1. A stale `brcm_patchram_plus` keeps `/dev/ttyTHS2` open — verify the port is
   truly free with `fuser` before every attempt (and don't let `tail` hide the
   output).
2. Piping into `head` loses buffered stdout when `timeout` kills the process —
   use `stdbuf -o0` or redirect to a file.
3. `sudo -S` consumes the first line of a heredoc as the password — upload the
   file separately, then `sudo cp`.
