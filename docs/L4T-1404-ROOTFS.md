# L4T 14.04 rootfs 快照（刷入即用备份）

> 适用：Xiaomi Mi Pad 1 (mocha / Tegra K1)，把 p29 (UDA) 上的 Ubuntu 14.04
> 恢复到「所有修复都已完成」的状态，**20 分钟内完成，不需要重新调试**。
>
> **这个快照是本仓库所有修复的最终成果**：蓝牙可用、apt 源已换、GPU 加速正常。

---

## 一、快照信息

### 当前版本（v2，2026-10-25 18:10）

| 项目 | 值 |
|---|---|
| 文件名 | `root-1404-v2.tar.gz` |
| 大小 | 1,741,593,604 字节（1.62 GiB） |
| SHA256 | `f94ed26cdf39774647b258f6e20bc1c251a680a8e141f328115ec797a9a8e25f` |
| 归档条目 | 153,071（文件 + 目录 + 软链） |
| 系统版本 | Ubuntu 14.04.6 LTS (Trusty) |
| 内核 | 3.10.108_SmokeR24.1-V3.5.1（预编译，未改） |
| 格式 | `tar`（gzip -1 压缩），**不是** cpio |
| 打包方式 | 在线打包（`/` 就是 p29:/l4t/root），tar 零警告，`gzip -t` 通过 |

**v2 相比 v1 多了什么：**

| 增量 | 说明 |
|---|---|
| **固定 BT MAC** | `bt-mac-setup.sh` v5，硬编码 `00:90:4C:16:70:02`，不再随 WiFi MAC 变化（见 `L4T-BLUETOOTH-FIX.md` 5.3） |
| **蓝牙开机确定性** | `/etc/init/nvwifibt.conf` 用 `/usr/sbin/rfkill`（原来写错成 `/sbin/rfkill` 会导致任务死循环） |
| **摄像头设备节点** | DTB 加 `num-channels = <2>` → `/dev/video0`、`/dev/video1`、`/dev/camera.pcl` 存在（见 `L4T-CAMERA-INVESTIGATION.md`） |
| `v4l-utils` / `strace` | 已安装，便于以后调试 |
| `/opt/camtest`（79 MB） | 摄像头实验沙盒：40 个 R21.5 库 + 无 daemon 版插件，以后拿到匹配的包可直接测试 |

> 体积比 v1 大 408 MB，主要是 `/var/cache/apt/archives`（328 MB）和
> `/var/lib/apt/lists`（80 MB）—— 可再生的 apt 缓存，保留可以离线重装。

### 历史版本（v1，2026-10-25 16:00）

| 项目 | 值 |
|---|---|
| 文件名 | `root-1404-final.tar.gz` |
| 大小 | 1,244,063,156 字节（1.16 GiB） |
| SHA256 | `906695d0941e8be3e31bc6acafcc4f93d213db7d7f407c472122ecdf55a240a3` |
| 归档条目 | 151,024（文件 + 目录 + 软链） |
| 普通文件数 | 87,660 |
| 解压后大小 | 约 2.7 GB |
| 系统版本 | Ubuntu 14.04.6 LTS (Trusty) |
| 内核 | 3.10.108_SmokeR24.1-V3.5.1（预编译，未改） |
| 格式 | `tar`（gzip -1 压缩），**不是** cpio |

---

## 二、快照里包含什么

| 内容 | 状态 |
|---|---|
| Ubuntu 14.04.6 LTS（已 `apt upgrade`） | ✅ |
| 4 核 / 2.9 GiB 可用内存 | ✅ |
| GPU 硬件加速（nvhost / tegra_dc） | ✅ |
| **蓝牙可用 + BD_ADDR 固定**（去 LPM + 硬编码 MAC） | ✅ 见 `L4T-BLUETOOTH-FIX.md` |
| **`/dev/video0`、`/dev/video1`、`/dev/camera.pcl`** | ✅ v2 起（采集仍缺 `camera_daemon`，见 `L4T-CAMERA-INVESTIGATION.md`） |
| apt 源 = 清华 TUNA（含 universe/multiverse） | ✅ |
| Unity 桌面 + compiz | ✅ |
| WiFi（bcmdhd） | ✅ |
| 触摸屏（atmel-maxtouch） | ✅ |
| 声音（tegra-rt5671） | ✅ |
| 摄像头实际采集 | ❌ 缺闭源 `camera_daemon` |

## 快照**不**包含什么

**启动文件不在归档里**，它们在 p29 的 `/l4t/boot/`，独立于 rootfs：

| 文件 | 大小 | SHA256 |
|---|---|---|
| `uImage-desktop` | 7,818,528 | `26c4c15a07678d44808736b9efab8276edf9c9a82cc15d006f251a6a2f5f6edc` |
| `mocha-desktop.dtb`（v2 用的，摄像头版） | 144,547 | `37941f97ca9b52476f8eff1b9bdf8407aee07225adfde2509ea09aa01da09777` |
| `mocha-desktop.dtb`（保守版，无摄像头改动） | 144,519 | `88fff3bc182ee6c91f12b4f2959d50f8671cca808553bf0000c6ebfddc5344f0` |
| `initramfs.cpio.gz` | 1,028,300 | `c79697cfd93afb346266504ecc66ae0756c7b54c96bec911f8ded03856571966` |

**只要这些文件还在，rootfs 换了也不影响启动。**

> ⚠️ 打包时 `/` 就是 p29 上的 `/l4t/root`，而 p29 又挂在 `/data` 和 `/mnt/p29`。
> 打包脚本必须排除 `./data`、`./mnt/*`，否则 tar 会递归进整个 rootfs。
> v2 已校验：归档里 `./data` 条目为 0。


---

## 三、恢复步骤

### 3.1 进 fastboot

```
关机 → 长按电源 30 秒 → 电源 + 音量减 按住 10-15 秒
```

### 3.2 启动 RAM 调试 U-Boot 和 RAM 安装器

```bash
# 主机上
fastboot boot artifacts/3gb/uboot/mocha-uboot-ramdebug-3gb.img
# 可能要重试几次：AdbWriteEndpointSync failed / 状态 121 是 USB 抖动

python tools/load-ram-nofb.py --serial 317454d236480e15 \
  --kernel artifacts/3gb/install/uImage-acm \
  --dtb    artifacts/3gb/install/mocha.dtb \
  --initrd artifacts/3gb/install/ram-install.cpio.gz
```

RAM 安装器的地址是 `172.31.124.2`。

### 3.3 上传归档

```bash
KEY=/root/mocha/maintenance-key/id_ed25519
SSH="ssh -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
U=root@172.31.124.2

# 挂载 p29
$SSH $U 'blockdev --setrw /dev/mmcblk0; mkdir -p /mnt
         mount | grep -q " /mnt " || mount /dev/mmcblk0p29 /mnt'

# 上传（约 1-2 分钟，速率 15 MB/s 左右）
cat artifacts/root-1404-v2.tar.gz | $SSH $U 'cat > /mnt/l4t/root-1404-v2.tar.gz'

# 校验（必须等于 906695d0...）
$SSH $U 'sha256sum /mnt/l4t/root-1404-v2.tar.gz'
```

### 3.4 确认启动文件还在（**关键**）

```bash
$SSH $U 'sha256sum /mnt/l4t/boot/uImage-desktop \
                   /mnt/l4t/boot/mocha-desktop.dtb \
                   /mnt/l4t/boot/initramfs.cpio.gz'
```

期望：

```
26c4c15a07678d44808736b9efab8276edf9c9a82cc15d006f251a6a2f5f6edc  uImage-desktop
88fff3bc182ee6c91f12b4f2959d50f8671cca808553bf0000c6ebfddc5344f0  mocha-desktop.dtb
c79697cfd93afb346266504ecc66ae0756c7b54c96bec911f8ded03856571966  initramfs.cpio.gz
```

⚠️ **如果 `mocha-desktop.dtb` 不匹配，先恢复 DTB**，否则会黑砖风险。

### 3.5 清空并解压

```bash
# 清空（只在归档校验通过之后！）
$SSH $U 'rm -rf /mnt/l4t/root/* /mnt/l4t/root/.[!.]*'

# 解压（用 busybox gzip，因为 RAM 环境里没有独立的 gzip 二进制）
$SSH $U 'mkdir -p /mnt/l4t/root && \
         cd /mnt/l4t/root && \
         busybox gzip -dc /mnt/l4t/root-1404-v2.tar.gz | tar -xf -'
```

约 8-12 分钟。

### 3.6 验证解压

```bash
$SSH $U 'head -2 /mnt/l4t/root/etc/os-release
         echo "files: $(find /mnt/l4t/root -type f | wc -l)"   # 期望 87660
         ls -l /mnt/l4t/root/sbin/init                          # 必须存在
         ls -l /mnt/l4t/root/usr/local/sbin/bt-mac-setup.sh     # 蓝牙修复'
```

### 3.7 卸载并重启

```bash
$SSH $U 'sync; sync; sync; umount /mnt; reboot -f'
```

---

## 四、恢复后验证清单

设备起来后：

```bash
# 1. 系统
lsb_release -d          # Ubuntu 14.04.6 LTS
uname -r                # 3.10.108_SmokeR24.1-V3.5.1
nproc                   # 4
grep MemTotal /proc/meminfo

# 2. 蓝牙（重点）
hciconfig -a
```

期望：

```
BD Address: 00:90:4C:11:22:34     ← 有效 MAC，不是 43:50:C0:00:00:00
UP RUNNING PSCAN ISCAN
Name: 'tegra-ubuntu-0'
Class: 0x00010c
```

```bash
# 3. HCI 是否响应
sudo hcitool cmd 0x03 0x0003      # 应返回 HCI Event 0x0e

# 4. 能否扫到设备
sudo hcitool lescan

# 5. apt 源
head -1 /etc/apt/sources.list     # 清华 TUNA
```

**手机应该能搜到 `tegra-ubuntu-0`。**

---

## 五、这个快照是怎么打的（可复现）

### 5.1 必须在 RAM 环境里打包

**不能在运行中的系统里打包**。之前试过 4 次都失败：

```
tar 在 100-165 MB 处被杀
原因：桌面负载 + [dram-timers] EMC 降频 → 系统卡到 OOM
```

### 5.2 ⚠️ `tar -z` 在 RAM 环境里会失败

```bash
tar -czf out.tar.gz .          # ✗ tar: can't execute 'gzip': No such file or directory
```

**原因**：RAM 安装器的 `/bin` 里没有独立的 `gzip` 二进制，
只有 busybox 的 applet。`tar -z` 内部用 `execvp("gzip")` 找真实文件，找不到。

**正确写法**（显式管道，让 shell 解析 applet）：

```bash
tar -cf - -C /mnt/l4t/root . | busybox gzip -1 > /mnt/l4t/root-1404-final.tar.gz
```

### 5.3 必须脱离 SSH 运行

RAM 环境的 SSH 会偶发断线。用 `setsid` 让进程脱离：

```bash
setsid sh -c 'tar -cf - -C /mnt/l4t/root . | busybox gzip -1 \
              > /mnt/l4t/root-1404-final.tar.gz' \
       > /tmp/pack.log 2>&1 < /dev/null &
```

⚠️ busybox **没有 `nohup`**，只能用 `setsid`。

---

## 5.5 v2 用的是「在线打包」（更简单，不用进 fastboot）

v1 是在 RAM 安装器里离线打包的。v2 发现**根本不需要进 fastboot**：

运行中的系统里 `/` **就是** p29 上的 `/l4t/root`，所以直接在线打包即可。
输出写到 `/mnt/p29/`（对应 p29 根目录，**不在 `/` 里面**，不会递归）。

```sh
tar -cf - -C / \
    --exclude=./proc/* --exclude=./sys/* --exclude=./dev/* \
    --exclude=./run/*  --exclude=./mnt/*  --exclude=./media/* \
    --exclude=./data   --exclude=./tmp/* \
    . 2> /mnt/p29/snap.err | gzip -1 > /mnt/p29/root-1404-v2.tar.gz
```

**⚠️ `--exclude=./data` 绝对不能漏！** p29 同时挂在 `/data`，里面就有
`/data/l4t/root` —— 不排除会让 tar 递归进整个 rootfs。

打包后用这三条验证：

```bash
gzip -t /mnt/p29/root-1404-v2.tar.gz              # 压缩流完整性
tar -tzf /mnt/p29/root-1404-v2.tar.gz | wc -l     # 条目数
tar -tzf /mnt/p29/root-1404-v2.tar.gz | grep -c '^\./data/'   # 必须是 0
```

> 注意 `rc=$?` 在 `tar | gzip` 管道里拿到的是 **gzip** 的状态，不是 tar 的。
> 要判断 tar 是否干净，看 `stderr` 是否为空（v2 是空的）。

实测：4.6 GB → 1.62 GiB，4 分 52 秒（gzip -1，K1 四核）。

### 5.4 监控用 `ps`，不要用 `pgrep -c`

busybox 的 `pgrep` 不可靠（进程还在时也可能返回 0）。用：

```bash
ps 2>/dev/null | grep -cE "[t]ar -cf|[g]zip -1"
```

### 5.5 实测数据

| 版本 | 源大小 | 压缩后 | 耗时 | 方式 |
|---|---|---|---|---|
| v1 | 2.7 GB / 87,660 文件 | 1.16 GiB（gzip -1） | 760 秒（约 13 分钟） | RAM 安装器离线打包 |
| v2 | 4.6 GB / 153,071 条目 | **1.62 GiB**（gzip -1） | **292 秒**（4 分 52 秒） | 在线打包（见 5.5 节上方） |

---

## 六、恢复时的常见错误

| 错误 | 原因 | 解决 |
|---|---|---|
| `tar -xzf` 报 `can't execute 'gzip'` | 同上，没有独立 gzip | `busybox gzip -dc ... \| tar -xf -` |
| `switch_root: can't execute '/sbin/init'` | 解压没完成（SSH 断了） | 重新解压，**用 setsid 脱离 SSH** |
| 开机黑屏 | `mocha-desktop.dtb` 被覆盖成带 pmoff 的版本 | 恢复 `88fff3bc...` 的 DTB |
| 搜不到设备 | BD_ADDR 还是占位符 | 看 `bt-mac-setup.sh` 是否执行（`hciconfig -a`） |
| 传输到一半断 | RAM 环境 SSH 抖动 | 校验字节数 + SHA256，不匹配就重传 |

---

## 七、和 16.04 快照的区别

| 项目 | 14.04（本文） | 16.04（`L4T-1604-ROOTFS.md`） |
|---|---|---|
| 格式 | tar + gzip | cpio + gzip |
| 大小 | 1.16 GiB | 2.4 GB |
| 蓝牙 | ✅ **可用** | ❌ tx timeout（LPM 问题，未在本快照修复） |
| 登录界面 | 14.04 原生（正常） | 需要 `lightdm-gtk-greeter` 等修复 |
| 稳定性 | ✅ 实测稳定 | ⚠️ 有多个已知问题 |
| 推荐度 | ⭐ **首选** | 仅作研究参考 |
