# NovaOS 架构设计（自研微内核 v0.1）

## 启动流程

```
BIOS
 └─> boot/boot.asm (stage1, 512B, 实模式)
      ├─ 设置段寄存器/栈 (0x7C00)
      ├─ CHS 读盘: loader.bin (扇区2-3) → 0x7E00
      ├─ CHS 读盘: kernel.bin (扇区4+)  → 0x10000
      └─ 跳转 0x7E00
      └─> boot/loader.asm (stage2, 实模式→保护模式)
           ├─ 开 A20 (fast gate, 端口0x92)
           ├─ 载入 flat GDT (code/data, 4GB)
           ├─ 切换 CR0.PE → 保护模式
           ├─ 拷贝 kernel.bin: 0x10000 → 0x100000
           ├─ 建页目录/页表: 恒等映射前 4MB
           ├─ 开 CR0.PG
           └─ 跳转 0x100000 → kernel_main (C)
```

## 内核子系统（kernel/）

| 模块 | 文件 | 功能 |
|---|---|---|
| GDT | gdt.c | 重建 flat GDT（内核拥有描述符表） |
| IDT | idt.c + isr_stubs.asm | 256 向量，int gate，异常/中断分发 |
| 中断 | isr.c | 异常报告（含页错误 CR2 输出）、IRQ 分发、EOI |
| PIC | irq.c | 8259 重映射 IRQ0-15 → 向量 32-47 |
| 时钟 | timer.c | PIT 100Hz，tick 计数，timer_wait |
| 键盘 | keyboard.c | 8042，扫描码→ASCII，shift/caps，环形缓冲 |
| 显示 | vga.c | 80x25 文本 + 320x200x256 图形（寄存器级初始化） |
| 串口 | serial.c | COM1 38400 8N1，内核日志输出 |
| 物理内存 | mm.c | 4KB 帧位图分配器（1MB 以下保留） |
| 内核堆 | kheap.c | first-fit 块分配器（1MB 堆区 0x300000-0x400000） |
| 分页 | paging.c | 恒等映射 4MB，1 页目录 + 1 页表 |
| 调度 | sched.c | 轮转调度，时间片，栈切换上下文 |
| 系统调用 | syscall.c | int 0x80：write/getpid/exit |
| 图形桌面 | gui.c | 帧缓冲绘制、窗口管理、任务栏、焦点切换 |
| Shell | shell.c | help/clear/meminfo/tasks/gui/echo/about/reboot |

## 内存布局

```
0x000000  实模式 IVT/BDA（保留）
0x000900  页目录
0x000A00  页表（恒等映射 4MB）
0x007C00  stage1 栈
0x007E00  stage2
0x010000  kernel.bin 暂存区
0x100000  内核（.text/.rodata/.data/.bss + 16KB 栈）
0x300000  内核堆 HEAP_START
0x400000  MEM_END（映射上限）
0xB8000   VGA 文本缓冲
0xA0000   VGA 图形帧缓冲（mode 13h）
```

## 系统调用 ABI

```
eax = 调用号, ebx/ecx/edx = 参数, int 0x80, 返回值在 eax
  0  sys_write(fd, buf, len)
  1  sys_getpid()
  2  sys_exit(code)
```

## 设计取舍（v0.1 明确不做）

- 单地址空间：内核与任务同处 4MB 恒等映射（无用户态隔离）→ 0.5 里程碑引入 ring3
- 无文件系统：镜像按固定扇区布局 → 0.5 里程碑引入 FAT/ext2 读取
- 无 ELF 加载器：任务以内核线程存在 → 0.5 里程碑引入用户进程
- 无多核/APIC：单 CPU 8259 PIC → 1.0 里程碑引入 SMP
