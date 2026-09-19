#!/bin/sh
# NovaOS - boot in QEMU with serial console mirroring the kernel log
set -e
cd "$(dirname "$0")"
make image
qemu-system-i386 -fda build/novaos.img -serial stdio -display sdl
