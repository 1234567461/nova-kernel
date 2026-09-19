# NovaOS 路线图（自研微内核 → 完整系统）

> 目标：“不是玩具”。每一里程碑都有可运行、可验证的交付物。

## v0.1 引导骨架 + 最小内核  ✅
- [x] 自研 stage1/stage2 引导（512B + 保护模式切换 + 分页）
- [x] GDT/IDT/PIC/PIT/键盘/串口/文本显示
- [x] 物理帧分配器 + 内核堆 + 恒等分页
- [x] 轮转调度（内核线程）+ int 0x80 系统调用
- [x] 内置 shell + mode13h 图形桌面（窗口/任务栏）
- 交付物：`make` → `build/novaos.img`，QEMU 引导，`gui` 命令进桌面

## v0.5 用户态 + 文件系统  ✅
- [x] 用户态进程：TSS + ring3 + int 0x80 gate（DPL3）+ 最小 ELF32 加载器
- [x] 内存保护：页表 U/S 位拆分（内核 supervisor / 用户窗口 0x200000-0x2FFFFF）
- [x] 文件系统：FAT12 读取（BPB/根目录/簇链）+ ATA PIO（secondary master）驱动
- [x] 桌面：PS/2 鼠标驱动（IRQ12）→ 光标、标题栏点击聚焦、窗口拖拽
- [x] 系统调用升级：getpid 返回真实 pid、exit 安全切走（不再 halt 内核）
- [x] 演示链路：启动自动从 FAT 盘加载 hello.elf → ring3 运行 → syscall → exit
- 交付物：`make` → `build/novaos.img` + `build/data.img`（FAT12 数据盘），
  QEMU `-fda novaos.img -fdb data.img`；shell 里 `ls` / `cat <file>` / `run <name>`

## v1.0 按进程页表 + COW  ✅（本仓库当前状态）
- [x] 每进程独立页目录 + 页表（内核区域继承映射，用户窗口私有）
- [x] fork() 系统调用（nr 3）：COW 克隆地址空间，父得子 pid、子得 0
- [x] 写时复制：共享页只读，#PF 处理复制帧（仅限 ring3 写）
- [x] 用户页引用计数（mm_ref_inc/dec）：最后一引用释放帧，进程 exit 回收地址空间
- [x] 调度器切换 CR3（用户任务间、用户/内核间）
- [x] 物理帧分配器保留区治理：避开内核镜像与堆，可用帧约 464 个
- 演示：hello.elf fork 后父子各自写 `.data` 全局变量 → 触发 COW → 各自独立副本
- 交付物：`make` → `build/novaos.img` + `build/data.img`，`run hello` 看 fork/COW 输出
- 明确延后：VBE 高分辨率、独立内核页表（目前用户页表继承内核 PTE）、exec/地址空间替换

## v1.5 自研图形栈 + 网络（下一步）
- [ ] VBE 高分辨率线性帧缓冲（1024x768x32）
- [ ] 自研合成器/窗口管理器（多窗口、分层合成）
- [ ] RTC 实时钟、FAT16/32、ATA DMA
- [ ] NE2000/RTL8139 网卡驱动 + 最小 TCP/IP 协议栈（ARP/ICMP/UDP/TCP）
- [ ] 自研 shell 强化（管道、重定向、环境变量）
- 验收：内核自带终端可 telnet/HTTP 拉取数据；桌面可多窗口叠放

## v2.0 自举工具链 + 应用
- [ ] 自研编译器前端（TinyCC 移植或自研子集）→ 内核态编译 C
- [ ] 包管理：自研 .npkg 格式 + 安装器
- [ ] 应用：文件管理器、文本编辑器、像素画板
- 验收：在 NovaOS 桌面内编译并运行用户程序（自举）

## v3.0+（远期）
- SMP 多核、APIC、虚拟内存完整化、POSIX 兼容层、蓝牙/触屏手机版内核

## 与 novaos-linux 的关系
自研内核是“理解底层”的实验室；`novaos-linux` 是“今天就能用”的生产系统。
两条线的驱动代码、UI 设计经验会互相借鉴。
