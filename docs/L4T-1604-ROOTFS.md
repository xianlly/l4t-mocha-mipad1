# L4T 16.04 rootfs 快照（可复用备份）

> 这是 **Ubuntu 16.04.7 LTS + L4T 3.10.108 内核** 的完整 rootfs 快照，
> 打包自一台**已升级成功且桌面可用**的小米平板 1（mocha）。

---

## 一、快照信息

| 项目 | 值 |
|---|---|
| **文件** | `artifacts/root-1604.tar.gz` |
| **大小** | 2.4 GB（解压后约 4.5 GB） |
| **文件数** | 179,948 |
| **格式** | `gzip` 压缩的 `cpio -H newc` 归档（**不是 tar**） |
| **SHA256** | `97056aca33e0fce41c8d4e69bd2f98e329b756c982b549b50956c84684727abc` |
| **系统版本** | Ubuntu 16.04.7 LTS (Xenial) |
| **内核** | 3.10.108_SmokeR24.1-V3.5.1 |
| **打包时间** | 2026-10-25 01:51 |

### 快照时的系统状态

```
PID 1:       upstart（14.04 的 init 仍在内存中运行）
桌面:        Unity，正常工作
greeter:     unity-greeter 能正常渲染（截图 986 KB）
用户:        ubuntu / ubuntu
内存识别:    2.9 GiB（4 核）
GPU:         nvhost-gpu + tegra_dc_0
```

⚠️ **重要**：这个快照是**「升级完成但还没重启」**的状态。

---

## 二、为什么要这个快照

16.04 升级要跑 **1 小时**。如果系统坏了，从 14.04 重新升级又是 1 小时。

有了这个快照：
```
恢复时间:  约 20 分钟
成功率:    100%（快照时是可用状态）
```

---

## 三、如何用它恢复

### 前置条件

- 设备能进 **fastboot**
- 主机上有这个 `root-1604.tar.gz`
- 启动文件（`/l4t/boot`）**不需要动**

### 步骤

#### 1. 设备进 fastboot

```
长按电源 30 秒 → 等 5 秒 → 电源 + 音量减 按住
```

#### 2. 引导 RAM 环境

```bash
# 主机执行
fastboot boot artifacts/3gb/uboot/mocha-uboot-ramdebug-3gb.img

# 等 RAM U-Boot 起来（fastboot devices 能看到 317454d236480e15）
python3 tools/load-ram-nofb.py \
  --serial 317454d236480e15 \
  --kernel artifacts/3gb/install/uImage-acm \
  --dtb    artifacts/3gb/install/mocha.dtb \
  --initrd artifacts/3gb/install/ram-install.cpio.gz
```

#### 3. 上传快照到设备

```bash
# RAM 安装器的 IP 是 172.31.124.2
scp -i <key> artifacts/root-1604.tar.gz root@172.31.124.2:/tmp/
```

⚠️ 2.4 GB 传输需要几分钟。

#### 4. 清空并恢复

```bash
ssh -i <key> root@172.31.124.2 '
  mount /dev/mmcblk0p29 /mnt
  rm -rf /mnt/l4t/root/*
  rm -rf /mnt/l4t/root/.[!.]*
  cd /mnt/l4t/root
  gzip -dc /tmp/root-1604.tar.gz | cpio -idmv --no-absolute-filenames
  echo "restore done"
'
```

#### 5. 重建 WiFi 固件软链

```bash
ssh -i <key> root@172.31.124.2 '
  mkdir -p /mnt/l4t/root/etc/firmware
  ln -sf /lib/firmware/fw_bcmdhd.bin /mnt/l4t/root/etc/firmware/fw_bcmdhd.bin
  ln -sf /lib/firmware/nvram.txt     /mnt/l4t/root/etc/firmware/nvram.txt
'
```

#### 6. 卸载并重启

```bash
ssh -i <key> root@172.31.124.2 'sync; sync; umount /mnt; reboot -f'
```

---

## 四、恢复后的状态

| 项目 | 状态 |
|---|---|
| 系统 | Ubuntu 16.04.7 ✓ |
| 内核 | 3.10.108（原版） |
| CPU / 内存 | 4 核 / 2.9 GiB ✓ |
| GPU | 硬件加速 ✓ |
| 桌面 | Unity ✓ |
| 防黑砖 | 取决于 `/l4t/boot` 里的 DTB |
| WiFi | 需要重新连接（密码不保留） |
| SSH | 密码登录 `ubuntu`/`ubuntu` |

---

## 五、已知问题（快照状态下）

### 1. 关机会报错

```
Failed to start poweroff.target: Connection timed out
```

**原因**：`upstart` 是 PID 1，但 `/sbin/init` 已指向 `systemd`。两套初始化系统冲突。

**绕过**：用 `sudo halt` 或长按电源键，或用 sysrq：

```bash
echo 1 | sudo tee /proc/sys/kernel/sysrq
echo o | sudo tee /proc/sysrq-trigger    # 关机（走 halt，防黑砖有效）
```

### 2. 重启后可能黑屏

**原因**：重启后 PID 1 变成 `systemd`，greeter/会话的启动方式完全改变。

这是**本次未解决的核心问题**。快照的价值在于：恢复后可以反复实验 systemd 启动问题，不用每次等 1 小时升级。

---

## 六、打包方法（可复现）

```bash
# 在 RAM 安装器环境（busybox 工具）里：
cd /mnt/l4t/root
find . \( -name proc -o -name sys -o -name dev -o -name tmp \
       -o -name run -o -name mnt -o -name media -o -name data \
       -o -name android -o -name snap -o -name lost+found \) -prune \
     -o -print0 \
  | cpio -0 -o -H newc \
  | gzip -1 > /path/to/root-1604.tar.gz
```

### 为什么必须在 RAM 环境打包

在运行的 16.04 系统里打包，`tar` 会在 100-165 MB 时被杀：

| 尝试 | 方式 | 结果 |
|---|---|---|
| 1 | `tar -czf` + setsid | 101 MB 死 |
| 2 | `tar -czf` + screen | 119 MB 死 |
| 3 | `tar -czf` + 完整排除 | 107 MB 死 |
| 4 | `tar -cf` + 外部 gzip | 165 MB 死 |
| **5** | **RAM 环境 + `find`/`cpio`** | **2.4 GB 成功** ✓ |

原因推测：桌面进程 + 内存压力导致 `[dram-timers]`（Tegra EMC）反复降频，进而拖死 tar。

### 恢复命令（对应上面的 cpio 格式）

```bash
gzip -dc root-1604.tar.gz | cpio -idmv --no-absolute-filenames
```

**不能**用 `tar -xzf`，格式不同。

---

## 七、校验

```bash
sha256sum root-1604.tar.gz
# 97056aca33e0fce41c8d4e69bd2f98e329b756c982b549b50956c84684727abc

# 文件数
gzip -dc root-1604.tar.gz | cpio -t | wc -l
# 179948
```
