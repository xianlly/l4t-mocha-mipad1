# MIUI 前置/后置 CSI 寄存器实测（2026-10-30）

在 MIUI9（Android 4.4.4）上，用静态 ARM `csidump` 通过 `/dev/mem` 抓的
`0x54080000..0x54082000`（T124 VI + CSI）。前置 OV5693 预览、后置 IMX179
预览各一份。

文件：

| 文件 | 内容 |
|---|---|
| `csi-front.txt` | 前置 OV5693 预览中 |
| `csi-back.txt` | 后置 IMX179 预览中 |
| `mipi-front.txt` | 前置时 MIPI_CAL（0x700e3000）|
| `dmesg-front.txt` | 前置时的 dmesg |
| `dmesg-back.txt` | 后置时的 dmesg |

抓取命令（MIUI root）：

```bash
adb push release/artifacts/csidump /data/local/tmp/
adb shell su -c "chmod 755 /data/local/tmp/csidump"
adb shell su -c "/data/local/tmp/csidump" > csi-front.txt
adb shell su -c "/data/local/tmp/csidump mipi" > mipi-front.txt
```

## 关键寄存器对比

| 地址 | 含义 | 前置（OV5693） | 后置（IMX179） |
|---|---|---|---|
| `0x54080908` | `PHY_CIL_COMMAND` | `0x10000000` | `0x00000101` |
| `0x5408092c` | CIL A pad | `0x00000007` | `0x00010000` |
| `0x54080934` | CIL A control | `0x00000002` | `0x00000007` |
| `0x5408093c` | CIL A status | `0x00000000` | `0x00000010` |
| `0x54080960` | CIL B pad | `0x00000007` | `0x00000000` |
| `0x54080968` | CIL B control | `0x00000002` | `0x00000007` |
| `0x54080970` | CIL B status | `0x00000000` | `0x00000010` |
| `0x54080a08` | CIL E pad | `0x00000000` | `0x00000000` |
| **`0x54080a10`** | **CIL E control** | **`0x00000009`** | `0x00000009` |
| **`0x54080a18`** | **CIL E status** | **`0x00000110`** | `0x00000110` |
| `0x54080838` | PP A control | `0x00ff0004` | `0x007f0017` |
| `0x5408086c` | PP B control | `0x007f0014` | `0x007f0014` |
| `0x54080af4` | `CLKEN_OVERRIDE` | `0` | `0` |

## 结论

1. **MIUI 的 CIL E 在 `0xa08/0xa10/0xa18`，不是 `0x192c/0x1934/0x193c`。**
   L4T R24.1 的 `camera/registers.h` 是 T210 布局：
   - `CIL_OFFSET=0xf4`
   - `PIXEL_PARSER_4_BASE=0x1838`
   - `PHY_CIL_COMMAND=0xd0`
   - `CLKEN_OVERRIDE=0x218`

   算出来 CIL E = `0x192c`，**实际在 `0xa08`**。所以 L4T 一直读写空洞，
   CSIE 永远返回 0。

2. **CSIA 之前“看起来能用”是巧合**：`0x838 + 0xd0 == 0x908`，刚好撞上
   全局 `PHY_CIL_COMMAND`；`0x838 + 0xf4 == 0x92c`，刚好是 CIL A。

3. 前置（CSI E，1 lane）的 `PHY_CIL_COMMAND` 是 `0x10000000`，后置
   （CSI A/B，4 lane）是 `0x00000101`。MIUI `vi2.c` 对 CSI E 1 lane 写的是
   `0x12020202`。

4. 正确 T124 映射：

```
PHY_CIL_COMMAND      0x908
CLKEN_OVERRIDE       0xaf4
CSI_SW_STATUS_RESET  0xaf0
CIL A                0x92c   (control +0x8, status +0x10)
CIL B                0x960
CIL C                0x994
CIL D                0x9c8
CIL E                0xa08   (control 0xa10, status 0xa18)
PP A                 0x838
PP B                 0x86c
```

对应内核修复：`release/patches/t124-csi-register-map.patch`，
内核 `kernel-camfix36-uImage`。
