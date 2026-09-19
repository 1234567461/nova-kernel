# NovaOS Kernel — 自研操作系统内核

从零手写（无 GRUB、无 Linux 源码、无现成内核代码）的 x86 32 位微内核：
**自研引导 → 保护模式 → 分页 → 中断 → 内存管理 → 调度 → 系统调用 → 图形桌面**，
全部代码在本仓库，`make` 一条命令产出可引导的 1.44MB 软盘镜像。

## 这不是玩具

- 真实可引导：512B 引导扇区（0x55AA 签名）+ stage2 保护模式切换 + 恒等分页
- 真实内核子系统：GDT / IDT / PIC / PIT / 键盘 / 鼠标 / VGA / 串口 / 物理帧分配 /
  内核堆 / 分页 / 轮转调度 / int 0x80 系统调用
- 真实用户态（v0.5）：ring3 进程 + TSS + ELF32 加载器 + U/S 页表保护
- 真实文件系统（v0.5）：FAT12 读取 + ATA PIO 驱动（`ls` / `cat` / `run`）
- 内存隔离（v1.0）：**每进程独立页表 + copy-on-write fork**，调度切 CR3，
  用户页引用计数回收
- 自带图形桌面：320x200x256 图形模式，窗口管理器 + 任务栏 + 鼠标拖拽 + 键盘交互
- 自带 shell：help / clear / meminfo / tasks / gui / echo / about / reboot / ls / cat / run
- 零警告构建，镜像布局经过字节级验证

## 如何克隆这个仓库（小白版）

git 就是把整个仓库（所有代码和历史）复制到你电脑上的工具。三分钟搞定：

**第 1 步：安装 git**
- Windows：去 https://git-scm.com/download/win 下载安装（一路下一步即可）
- macOS：打开“终端”，输入 `xcode-select --install`，按提示装
- Linux（Ubuntu/Debian）：终端输入 `sudo apt install -y git`

**第 2 步：复制仓库地址**
本仓库地址：`https://github.com/1234567461/nova-kernel.git`

**第 3 步：克隆**
打开终端 / 命令行，输入：

```sh
git clone https://github.com/1234567461/nova-kernel.git
cd nova-kernel
```

完成！以后想更新到最新版，进到文件夹里输入 `git pull` 即可。

**只看不下载**：直接在浏览器打开 https://github.com/1234567461/nova-kernel ，
点绿色的 `<> Code` 按钮，还能选“Download ZIP”打包下载。

## 快速开始

```sh
make                          # 构建 build/novaos.img（零警告）
make data                     # 构建 FAT12 数据盘 build/data.img
qemu-system-i386 -fda build/novaos.img -fdb build/data.img -serial stdio
```

- 启动自动从数据盘加载 `hello.elf` 到 ring3 运行（v1.0 演示 fork + COW）
- shell 里输入 `ls` 看文件、`cat HELLO.TXT` 读文件、`run hello` 再跑一次用户程序、
  `gui` 打开图形桌面（`Tab` 切窗口，`Esc` 关闭，`C/A/M` 开窗口，鼠标可拖拽）

> 需要 QEMU：Linux `sudo apt install qemu-system-x86`；Windows 到 qemu.org 下载。

## 仓库结构

```
boot/      自研引导：stage1 引导扇区 + stage2 保护模式/分页跳转
kernel/    内核：GDT IDT 中断 PIC 时钟 键盘 鼠标 VGA 串口 内存 堆 分页 调度 系统调用 用户态 GUI shell
libc/      自研 C 库雏形：string + printf
userspace/ 用户态程序示例（int 0x80 ABI，fork/COW 演示）
tools/     数据盘生成器（mkdata.py，FAT12）
docs/      需求分析 / 架构 / 路线图 / 构建说明
```

## 文档

- `docs/REQUIREMENTS.md` — 完整 Linux 系统由什么组成（调研结论，两个仓库的分工依据）
- `docs/ARCHITECTURE.md` — 启动流程、子系统、内存布局、系统调用 ABI
- `docs/ROADMAP.md` — v0.1 → v3.0 路线图（当前 v1.0 已完成）
- `docs/BUILD.md` — 构建与验证

## 里程碑状态

- ✅ v0.5 用户态进程（ring3 + ELF 加载器）+ FAT12 文件系统 + PS/2 鼠标桌面
- ✅ v1.0 按进程独立页表 + copy-on-write fork（当前版本）
- ⏭ v1.5 VBE 高分辨率自研合成器 + 网卡驱动 + TCP/IP
- ⏭ v2.0 自举工具链 + 包管理。详见 `docs/ROADMAP.md`

> 配套仓库：`novaos-linux` —— 基于上游的完整桌面/手机发行版构建工程。
