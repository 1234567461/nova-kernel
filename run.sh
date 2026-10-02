#!/bin/sh
# NovaOS - boot in QEMU with serial console mirroring the kernel log
set -e
cd "$(dirname "$0")"
make image
qemu-system-i386 -fda build/novaos.img \
    -drive file=build/data.img,format=raw,if=ide,index=2 -boot a \
    -serial stdio -display sdl
