# NovaOS 系统需求分析：一个完整 Linux 系统由什么组成？

> 本文件是调研结论，直接决定 `nova-kernel`（自研）与 `novaos-linux`（上游）两个仓库各自实现什么。
> 参考来源：
> - https://thelinuxbook.com/chapter2-linuxmadeof
> - https://sunhick.github.io/posts/linux-kernel-xorg-gnome-from-scratch/
> - https://sunhick.github.io/posts/linux-boot-to-desktop-integration/
> - https://wiki.postmarketos.org/wiki/Mainlining_Guide
> - https://wiki.postmarketos.org/wiki/Porting_to_a_new_device

## 1. 一个完整桌面 Linux 系统的分层（自上而下）

| 层 | 职责 | 代表实现（上游） | 自研仓库对应（nova-kernel） |
|---|---|---|---|
| 引导层 | 上电后第一段代码，初始化 CPU/内存，加载内核 | GRUB / systemd-boot | `boot/boot.asm` + `boot/loader.asm`（自研 stage1/stage2） |
| 内核 | 进程、内存、驱动、中断、系统调用 | Linux 6.x | `kernel/`（GDT/IDT/分页/分配器/调度/系统调用） |
| C 库 | 应用与内核之间的标准接口 | glibc / musl | `libc/`（string/printf 雏形） |
| init/服务 | 启动进程、拉起服务、会话管理 | systemd / OpenRC | `userspace/init.c`（待完善） |
| 显示服务器 | 把应用的绘制命令变成屏幕像素 | Wayland / Xorg | `kernel/gui.c`（mode 13h 帧缓冲 + 自研窗口管理） |
| 桌面环境 | 窗口、面板、文件管理、设置、启动器 | GNOME / KDE / XFCE | `kernel/gui.c`（窗口/任务栏雏形） |
| 包管理器 | 安装/更新/卸载软件 | apt / dnf / apk | 路线图（自研包格式） |
| 应用生态 | 浏览器、编辑器、办公…… | Firefox / VSCode / LibreOffice | 路线图 |

**关键链路（应用→屏幕）**：
`应用 → 图形库(Mesa) → 显示服务器(合成器) → DRM/KMS → 内核 → 显存 → 屏幕`
自研内核里这条链路简化为：`gui.c 绘制 → 直接写 VGA 帧缓冲 → 屏幕`，这是链路的最短自研版本。

## 2. 手机 Linux 系统（postmarketOS 机制）需要什么？

postmarketOS 与 Android 完全不同：不用 Android 构建系统/HAL/用户空间，而是
**Alpine Linux（musl + BusyBox + apk-tools）** 直接跑在主线上游内核上。

| 组件 | 作用 | novaos-linux 仓库对应 |
|---|---|---|
| Alpine 基座 | 极小的根文件系统（约 5-6MB） | `mobile/` 构建脚本基于 Alpine |
| 设备专属内核包 | `linux-<厂商>-<机型>`，打补丁构建内核 | `mobile/devices/<设备>/APKBUILD` |
| 设备树 DT | 描述板级硬件，主线上游维护 | `mobile/devices/<设备>/` |
| UI 层 | Phosh / Plasma Mobile / Sxmo 三选一 | `mobile/ui/` |
| 外设配置 | udev 规则、ALSA UCM 声卡配置 | `mobile/scripts/` |

## 3. 结论：两个仓库的分工

- **nova-kernel（自研）**：验证“从零能不能长出操作系统”。当前交付可引导、可交互、
  自带图形桌面的微内核雏形（真实代码、真实编译、真实引导），完整目标按
  `docs/ROADMAP.md` 推进。
- **novaos-linux（上游）**：交付“拿来即用、可装机”的完整系统。桌面用 Debian
  live-build 定制发行版；手机用 postmarketOS 移植框架。见该仓库 `docs/`。

> 坦率说明：自研仓库当前是**可运行的微内核雏形**（约 2000 行，可编译成
> 1.44MB 镜像、QEMU 可引导、有 shell 和图形桌面），不是生产级系统——
> “完整系统”的每一层都在 ROADMAP 中有明确的实现计划和验收标准。
