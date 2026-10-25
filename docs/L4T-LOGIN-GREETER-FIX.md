# L4T (Ubuntu 16.04) 登录界面修复指南

> 适用：Xiaomi Mi Pad 1 (mocha) 上从 Ubuntu 14.04 升级到 16.04 后，出现黑屏、登录界面不显示、登录后会话自动退出等问题。
>
> **本文所有配置均在本机实测验证通过。**

---

## 一、最终可用配置（直接抄）

### `/etc/lightdm/lightdm.conf`

```ini
[Seat:*]
autologin-user=ubuntu
autologin-user-timeout=0
autologin-session=ubuntu
greeter-session=lightdm-gtk-greeter
```

### `/etc/lightdm/lightdm-gtk-greeter.conf`

```ini
[greeter]
background=/usr/share/backgrounds/warty-final-ubuntu.png
theme-name=Ambiance
icon-theme-name=ubuntu-mono-dark
font-name=Ubuntu 11
xft-antialias=true
xft-hintstyle=hintslight
indicators=~host;~spacer;~clock;~spacer;~session;~power;~a11y
show-clock=true
clock-format=%H:%M
show-manual-login=true
hide-users=false
show-guest=false
a11y-states=keyboard;contrast;font;reader
keyboard=onboard
keyboard-position=bottom
```

### 需要安装的包

```bash
sudo apt-get install -y lightdm-gtk-greeter onboard consolekit
sudo gpasswd -a ubuntu nopasswdlogin
```

---

## 二、五个关键发现（按重要性排序）

### 发现 1：`indicators=` 必须包含 `~a11y` ⭐⭐⭐

**这是最隐蔽的坑。**

```
网上大多数教程给的:
  indicators=~host;~spacer;~clock;~spacer;~session;~power

结果: ♿ 辅助功能菜单里没有 "On Screen Keyboard"

正确写法:
  indicators=~host;~spacer;~clock;~spacer;~session;~power;~a11y
                                                         ↑ 必须有
```

**如何自己验证**：

```bash
strings /usr/sbin/lightdm-gtk-greeter | grep -E '~a11y|a11y-states|keyboard'
```

会看到：

```
a11y-states
keyboard
keyboard_menuitem
keyboard-position
onboard
~a11y            ← 关键
a11y_keyboard_cb
```

### 发现 2：`lightdm-gtk-greeter` 在 systemd 下**能正常渲染** ⭐⭐⭐

这是对之前结论的纠正：

| greeter | systemd 下 | 备注 |
|---|---|---|
| `unity-greeter` | ❌ 黑屏 | 依赖 GNOME 会话服务，画不出来 |
| **`lightdm-gtk-greeter`** | ✅ **正常** | 纯 GTK2 绘制，不依赖硬件合成 |
| `gdm3` + `gnome-shell` | ❌ 黑屏 | 需要 X ≥ 1.18，L4T 是 1.15 |

**所以：用 `lightdm-gtk-greeter` 就够了，不需要切 upstart，不需要 gdm3。**

### 发现 3：`window-stack-bridge` 必须用真二进制 ⭐⭐⭐

16.04 把这个文件改名成 `hud-service`，但 upstart job 仍指向旧名：

```sh
# /usr/share/upstart/sessions/window-stack-bridge.conf
exec /usr/lib/arm-linux-gnueabihf/hud/window-stack-bridge
```

**不能用软链把 `hud-service` 冒充 `window-stack-bridge`**——两者功能不同：

| 文件 | 大小 | 功能 |
|---|---|---|
| `hud-service`（16.04） | 414 KB | HUD 服务（`com.canonical.hud`） |
| **`window-stack-bridge`（14.04）** | **126 KB** | 真正的窗口栈桥接器 |

**真二进制需从 14.04 的 `root_00.tar.gz` 提取**，同时需要它的依赖库：

```bash
# 从 14.04 rootfs 提取
tar -xzf root_00.tar.gz -C /tmp/l4t14 \
  ./usr/lib/arm-linux-gnueabihf/hud/window-stack-bridge \
  ./usr/lib/arm-linux-gnueabihf/libubuntu_application_api.so.1.0.0

# 安装到 16.04 rootfs
install -m 755 /tmp/l4t14/usr/lib/arm-linux-gnueabihf/hud/window-stack-bridge \
  /usr/lib/arm-linux-gnueabihf/hud/window-stack-bridge
install -m 644 /tmp/l4t14/usr/lib/arm-linux-gnueabihf/libubuntu_application_api.so.1.0.0 \
  /usr/lib/arm-linux-gnueabihf/
ln -sf libubuntu_application_api.so.1.0.0 /usr/lib/arm-linux-gnueabihf/libubuntu_application_api.so.1
ldconfig
```

**缺少这个 → 进桌面 30 秒后自动退回登录界面**（`unity-settings-daemon` 崩溃 → `gnome-session` 判定必需组件死亡 → 会话退出）。

### 发现 4：不要创建 `org.gnome.SessionManager.service` ⛔

**这是我自己踩过的坑，会导致 30 秒退出。**

```bash
# ⛔ 绝对不要创建这个文件！
/usr/share/dbus-1/services/org.gnome.SessionManager.service
```

**原因**：`gnome-session` 不是 D-Bus 按需激活的服务。创建这个文件后，D-Bus 会在 greeter 阶段启动一个「野」`gnome-session` 实例，抢走 `org.gnome.SessionManager` 这个总线名。真正的会话启动时无法获得该名字 → 判定会话管理器初始化失败 → 退出。

**正常系统中这个文件根本不存在。**

### 发现 5：`ubuntu` 用户会从登录列表里消失

因为自动登录需要把用户加入 `nopasswdlogin` 组：

```bash
sudo gpasswd -a ubuntu nopasswdlogin
```

而 `lightdm-gtk-greeter` **默认会隐藏 `nopasswdlogin` 组里的用户**（安全考虑）。结果登录界面只显示 "Guest Session"。

**修复**：

```ini
hide-users=false
show-manual-login=true
show-guest=false
```

---

## 三、症状 → 根因对照表

| 症状 | 根因 | 修复 |
|---|---|---|
| **进桌面约 30 秒后退回登录页** | `window-stack-bridge` 缺失 | 装真二进制（发现 3） |
| 同上（另一种情况） | 创建了 `org.gnome.SessionManager.service` | 删掉它（发现 4） |
| ♿ 菜单里没有 "On Screen Keyboard" | `indicators=` 缺 `~a11y` | 补上（发现 1） |
| 登录界面只显示 "Guest Session" | 用户在 `nopasswdlogin` 组里被隐藏 | `hide-users=false` + `show-manual-login=true` |
| 登录界面黑屏（有鼠标） | 用了 `unity-greeter` | 换 `lightdm-gtk-greeter`（发现 2） |
| 注销后黑屏 | greeter 被 `pkill` 但 lightdm 不 respawn | `systemctl restart lightdm` |
| 切 gdm3 后 lightdm 起不来 | 两个文件都改了但只回退一个 | 两个都要回退（见第四节） |

---

## 四、显示管理器切换的正确做法（两个文件都要改）

### 切到 gdm3

```bash
echo /usr/sbin/gdm3 > /etc/X11/default-display-manager
ln -sf /lib/systemd/system/gdm3.service /etc/systemd/system/display-manager.service
systemctl daemon-reload
```

### 切回 lightdm（**两个文件都要回退**）

```bash
echo /usr/sbin/lightdm > /etc/X11/default-display-manager
ln -sf /lib/systemd/system/lightdm.service /etc/systemd/system/display-manager.service
systemctl daemon-reload
systemctl reset-failed lightdm
systemctl start lightdm
```

**为什么两个文件都要改**：`lightdm.service` 有个守卫检查：

```sh
ExecStartPre=/bin/sh -c '[ "$(basename $(cat /etc/X11/default-display-manager))" = "lightdm" ]'
```

如果 `/etc/X11/default-display-manager` 还是 `gdm3`，这个检查失败 → `status=1` → 反复重启 → `start-limit-hit` → lightdm 彻底起不来。

**`gdm.service` 有更严格的检查**（全路径精确匹配）：

```sh
ExecStartPre=/bin/sh -c '[ "$(cat /etc/X11/default-display-manager 2>/dev/null)" = "/usr/sbin/gdm3" ]'
```

---

## 五、为什么 gdm3 不可用（已验证）

虽然 gdm3 能启动，X 服务器也能正常初始化 GPU：

```
[177.534] NVIDIA(GPU-0): DFP-0: connected
[178.092] NVIDIA(0): Setting mode "DSI-0: nvidia-auto-select @1536x2048 +0+0"
```

但 **`gnome-shell --mode=gdm`（gdm 的 greeter）画不出来**，因为：

```
GNOME Shell 3.18 需要 X ≥ 1.18
L4T 的 libglx.so 只支持 X 1.15
```

| 选择 | 结果 |
|---|---|
| 保持 X 1.15 | GPU 加速可用，但 GNOME Shell 黑屏 |
| 升级 X 到 1.18 | GNOME Shell 正常，但 GPU 加速失效（退回 llvmpipe） |

**这是硬约束，除非重编 L4T 的图形驱动。**

---

## 六、屏幕键盘（onboard）在登录界面的启用

### 前提

```bash
sudo apt-get install -y onboard
```

### 配置

```ini
# /etc/lightdm/lightdm-gtk-greeter.conf
[greeter]
indicators=...;~a11y          # 必须有 ~a11y
a11y-states=keyboard;contrast;font;reader
keyboard=onboard
keyboard-position=bottom      # bottom / top / floating
```

### 使用

登录界面右上角 **♿** 图标 → 勾选 **"On Screen Keyboard"** → 屏幕底部弹出虚拟键盘。

### 调整键盘外观（可选）

```bash
sudo -u lightdm gsettings set org.onboard layout 'Compact'
sudo -u lightdm gsettings set org.onboard theme 'Nightshade'
```

---

## 七、验证清单

```bash
# 1. greeter 进程
ps -ef | grep [g]reeter
# 期望: /usr/sbin/lightdm-gtk-greeter

# 2. 会话稳定（等待 2 分钟后再查）
who
# 期望: ubuntu  tty7  ... (:0)

# 3. 关键错误是否消失
tail -20 ~/.xsession-errors
# 不应再有: "Failed to spawn window-stack-bridge"

# 4. 不该存在的文件
test ! -e /usr/share/dbus-1/services/org.gnome.SessionManager.service && echo OK

# 5. a11y 键盘配置
grep -E 'indicators|a11y-states|keyboard' /etc/lightdm/lightdm-gtk-greeter.conf

# 6. 全屏截图验证 greeter 是否渲染
sudo env DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 \
  import -window root /tmp/greeter.png
ls -l /tmp/greeter.png
# 555 字节 = 纯黑（失败）; 几百 KB = 正常渲染
```

---

## 八、已知限制

| 项目 | 状态 | 说明 |
|---|---|---|
| 摄像头 | ❌ | 无 `/dev/video*`，ISP 硬件节点存在但 V4L2 设备未创建 |
| GNOME Shell / gdm3 | ❌ | X 版本约束（见第五节） |
| 关机 | ⚠️ | 混合 init 状态下 `poweroff.target` 会报错；用 `halt` 或 sysrq |

---

## 九、参考

- 本机验证：Ubuntu 16.04.7 LTS + 内核 3.10.108_SmokeR24.1（L4T R24.1）
- LightDM 1.18.3 / lightdm-gtk-greeter 2.0.1 / onboard 1.2.0
- 升级源帖：[How to successfully update Jetson TK1 to Ubuntu 16.04](https://forums.developer.nvidia.com/t/how-to-successfully-update-jetson-tk1-to-ubuntu-16-04/62174)
