# 构建说明

## 依赖
- GNU as（binutils，gcc 自带）、gcc（支持 -m32）、ld（支持 -m elf_i386）、
  make、objcopy、dd、xxd
- 运行验证：qemu-system-i386（可选，建议安装）

## 构建
```sh
make            # 产出 build/novaos.img（1.44MB 可引导镜像）
make clean      # 清理
```

## 运行（QEMU）
```sh
qemu-system-i386 -fda build/novaos.img -serial stdio
```
- 串口镜像内核日志（-serial stdio）
- VGA 窗口显示 shell；输入 `help` 看命令，`gui` 进入图形桌面
- 图形桌面按键：`Tab` 切换窗口焦点，`Esc` 关闭当前窗口，
  `C`/`A`/`M` 打开 Console/About/Memory 窗口

## 镜像布局
```
扇区 0      : boot.bin（512B，0x55AA 签名）
扇区 1-2    : loader.bin（≤1024B）
扇区 3..end : kernel.bin（≤96 扇区 = 49152B）
```

## 在本环境验证的结果
- `make`：零警告编译链接通过
- boot.bin = 512B（签名 55AA）；loader.bin = 190B；kernel.bin = 20624B
- `kernel_main` 位于 0x1027D7，.text 起始 0x100000（与 link.ld/loader.asm 约定一致）
- 本机无 qemu，未做实时引导验证；在有 qemu 的机器上直接跑上面的命令即可
