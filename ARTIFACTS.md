# 产物说明 / Artifact manifest

本目录下的二进制是「从原版 L4T 包 + 本仓库脚本」生成的**中间产物**，仅为方便复现而附带。
请务必先阅读下面的来源与许可说明。

The binaries in `artifacts/` are derived from the original L4T package plus the scripts in this
repository. They are included for convenience only — read the provenance and licensing notes.

---

## `artifacts/dtb-final-3gb-nopmoff.dtb`

| 项 | 值 |
|---|---|
| SHA256 | `88fff3bc182ee6c91f12b4f2959d50f8671cca808553bf0000c6ebfddc5344f0` |
| 大小 | 144,519 bytes |
| 节点数 | 887（与原版一致） |

**生成方式**：`scripts/03-build-dtb-3gb.sh` → `scripts/04-build-dtb-nopmoff.sh`

**内容**：
1. `memory@0x80000000` 改为 2000 MiB 低区 + 1024 MiB 高区（3 GB 机型可用 2.9 GiB）
2. 删除 `power_pm` 节点的 `system-pmic-power-off` 属性（电源键修复）

**基线**：原版 `tegra124-mocha.dtb`
（SHA256 `76233bdaf1552075284475b388af50e3c824d98a21ab85cb6ef0b6a4b93d7006`，144,537 bytes）

---

## `artifacts/initramfs-mocha-data.cpio.gz`

| 项 | 值 |
|---|---|
| SHA256 | `c79697cfd93afb346266504ecc66ae0756c7b54c96bec911f8ded03856571966` |
| 大小 | 1,028,300 bytes |

**生成方式**：`scripts/01-patch-initramfs.sh`

**改动**：分区号从 Shield Tablet 换成 mocha：
`DATAPART=p29  SYSPART=p26  FACTORYPART=p18  CALIBPART=p8  CACHEPART=p28`，
`PARTOFF=0`，四个额外挂载改为失败不致命。

---

## `artifacts/kernel-original-uImage`

| 项 | 值 |
|---|---|
| SHA256 | `26c4c15a07678d44808736b9efab8276edf9c9a82cc15d006f251a6a2f5f6edc` |
| 大小 | 7,818,528 bytes |

**生成方式**：`scripts/02-mkimage-kernel.sh`

**内容**：原版 `zImage`（7,818,464 bytes，SHA256 `37c12f811aa619b87866f8124b0f1acb88a9f064d678bd1a7b41ce86ede236d3`）
用 `mkimage -C none -a/-e 0x80008000` 封装。

> ⚠️ **这是原厂 3.10.108 "SmokeR24.1-V3.5.1" 内核，不是我们自己编译的。**
> 已验证：重编内核要么编译失败、要么无法启动——不要试图替换它。

---

## `artifacts/uboot-l4t-3gb.img`

| 项 | 值 |
|---|---|
| SHA256 | `ca277399fde1b7b3015301ce4e2660a9c8c98819151978f2b069ca4dd30b1939` |
| 大小 | 569,344 bytes |

**生成方式**：`scripts/05-build-uboot.sh`（mocha-moze U-Boot + 改过的环境变量）

**关键点**：
- 从 `mmc 0#UDA`（即 p29）加载 `/l4t/boot/*`
- bootargs 用 `rom_info.txt` 的完整板级参数，去掉 `mem=…`，
  root 指向 `/dev/mmcblk0p29` + `rootsubdir=/l4t/root`

> 刷入前请自行核对：`strings uboot-l4t-3gb.img | grep -c 'tegraid=40.1.1.0.0'` 应为 1。

---

## 原版文件（需自行获取 / files you must obtain yourself）

这些是 NVIDIA L4T / 第三方项目的原始文件，**不在本仓库分发**：

| 文件 | SHA256 | 说明 |
|---|---|---|
| `tegra124-mocha.dtb` | `76233bda…d7006` | 在 `L4T_R241.zip:root_dir/boot/` |
| `zImage` | `37c12f81…e236d3` | 在 `L4T_R241.zip:root_dir/boot/` |
| `root_00.tar.gz` | `3df1633f…8f588` | 在 `L4T_R241.zip:root_dir/`，736,208,718 bytes |
| `initramfs.cpio.gz`（原始） | — | 在 `L4T_R241.zip:root_dir/boot/` |

---

## 许可提醒 / Licensing

* **U-Boot**（`uboot-l4t-3gb.img`）：GPL-2.0+，源码见 `mocha-moze-boot`。
* **DTB / initramfs**：包含 NVIDIA L4T R24.1 的数据，版权归 NVIDIA / 原移植者
  （Insei / arttttt）。**分发前请自行确认许可**，或改为只分发脚本、
  让使用者用自己下载的原版包生成。
* **文档**：CC BY-SA 4.0（建议）。
