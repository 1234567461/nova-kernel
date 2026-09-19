# NovaOS Kernel — 自研操作系统内核

从零手写（无 GRUB、无 Linux 源码、无现成内核代码）的 x86 32 位微内核：
**自研引导 → 保护模式 → 分页 → 中断 → 内存管理 → 调度 → 系统调用 → 图形桌面**，
全部代码在本仓库，`make` 一条命令产出可引导的 1.44MB 软盘镜像。

## 这不是玩具

- 真实可引导：512B 引导扇区（0x55AA 签名）+ stage2 保护模式切换 + 恒等分页
- 真实内核子系统：GDT / IDT / PIC / PIT / 键盘 / VGA / 串口 / 物理帧分配 /
  内核堆 / 分页 / 轮转调度 / int 0x80 系统调用
- 自带图形桌面：320x200x256 图形模式，窗口管理器 + 任务栏 + 键盘交互
- 自带 shell：help / clear / meminfo / tasks / gui / echo / about / reboot
- 零警告构建，镜像布局经过字节级验证

## 快速开始

```sh
make                          # 构建 build/novaos.img
qemu-system-i386 -fda build/novaos.img -serial stdio
```

进入 shell 后输入 `gui` 打开图形桌面（`Tab` 切窗口，`Esc` 关闭，`C/A/M` 开窗口）。

## 仓库结构

```
boot/      自研引导：stage1 引导扇区 + stage2 保护模式/分页跳转
kernel/    内核：GDT IDT 中断 PIC 时钟 键盘 VGA 串口 内存 堆 分页 调度 系统调用 GUI shell
libc/      自研 C 库雏形：string + printf
userspace/ 用户态程序示例（int 0x80 ABI）
docs/      需求分析 / 架构 / 路线图 / 构建说明
```

## 文档

- `docs/REQUIREMENTS.md` — 完整 Linux 系统由什么组成（调研结论，两个仓库的分工依据）
- `docs/ARCHITECTURE.md` — 启动流程、子系统、内存布局、系统调用 ABI
- `docs/ROADMAP.md` — v0.1 → v3.0 路线图（用户态、文件系统、网络、自举）
- `docs/BUILD.md` — 构建与验证

## 后续里程碑

v0.5 用户态进程（ring3 + ELF 加载器）+ FAT 文件系统；
v1.0 VBE 高分辨率自研合成器 + 网卡驱动 + TCP/IP；
v2.0 自举工具链 + 包管理。详见 `docs/ROADMAP.md`。

> 配套仓库：`novaos-linux` —— 基于上游的完整桌面/手机发行版构建工程。
