# ============================================================================
#  NovaOS self-made kernel - build system
#  Tools: GNU as (for boot/loader), gcc -m32 freestanding, ld, objcopy, dd
#  Output: build/novaos.img  - 1.44MB raw floppy image, BIOS-bootable
#          (run with: qemu-system-i386 -fda build/novaos.img -fdb build/data.img
#           -serial stdio)
# ============================================================================

AS      := as
CC      := gcc
LD      := ld
OBJCOPY := objcopy
DD      := dd

BUILD   := build
CFLAGS  := -m32 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -nostdlib \
           -mno-sse -mno-mmx -mno-80387 -Wall -Wextra -I. -Ikernel -Ilibc
LDFLAGS := -m elf_i386 -T link.ld -nostdlib

# kernel.bin may not exceed 200 sectors (102400 bytes) - matches boot.asm
KERNEL_MAX := 102400

C_SRCS := kernel/gdt.c kernel/idt.c kernel/isr.c kernel/irq.c \
          kernel/timer.c kernel/keyboard.c kernel/mouse.c kernel/vga.c \
          kernel/serial.c kernel/mm.c kernel/kheap.c kernel/paging.c \
          kernel/sched.c kernel/syscall.c kernel/user.c kernel/ata.c \
          kernel/fat.c kernel/gui.c kernel/shell.c kernel/kernel.c \
          libc/string.c libc/printf.c
C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
ASM_OBJS := $(BUILD)/kernel/isr_stubs.o
K_OBJS := $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run check data

all: image data

# --- boot sector + stage2 -----------------------------------------------
$(BUILD)/boot.bin: boot/boot.asm | $(BUILD)
	$(AS) --32 $< -o $(BUILD)/boot.o
	$(OBJCOPY) -O binary $(BUILD)/boot.o $@
	@test $$(stat -c%s $@) -eq 512 || (echo "ERROR: boot.bin != 512 bytes"; exit 1)
	@echo "boot.bin: $$(stat -c%s $@) bytes (sector 0)"

$(BUILD)/loader.bin: boot/loader.asm | $(BUILD)
	$(AS) --32 $< -o $(BUILD)/loader.o
	$(OBJCOPY) -O binary $(BUILD)/loader.o $@
	@test $$(stat -c%s $@) -le 1024 || (echo "ERROR: loader.bin > 1024 bytes"; exit 1)
	@echo "loader.bin: $$(stat -c%s $@) bytes (sectors 1-2)"

# --- kernel ---------------------------------------------------------------
$(C_OBJS): $(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/isr_stubs.o: kernel/isr_stubs.asm | $(BUILD)
	$(AS) --32 $< -o $@

$(BUILD)/kernel.elf: $(K_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(K_OBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(stat -c%s $@) -le $(KERNEL_MAX) || (echo "ERROR: kernel.bin too large"; exit 1)
	@echo "kernel.bin: $$(stat -c%s $@) bytes (sectors 3+)"

# --- ring-3 demo user program (static ELF32 at 0x200000) ----------------
$(BUILD)/userspace/hello.elf: userspace/hello.c userspace/user.ld | $(BUILD)
	mkdir -p $(BUILD)/userspace
	$(CC) -m32 -nostdlib -fno-pie -fno-stack-protector -fno-builtin \
	      -mno-sse -mno-mmx -mno-80387 -static \
	      -Wl,-T,userspace/user.ld -Wl,--build-id=none -o $@ $<
	@echo "hello.elf: user program ready"

# --- FAT12 data disk (attached as B: in QEMU) ---------------------------
data: $(BUILD)/data.img

$(BUILD)/data.img: $(BUILD)/userspace/hello.elf tools/mkdata.py | $(BUILD)
	python3 tools/mkdata.py $(BUILD)/data.img $(BUILD)/userspace/hello.elf

# --- assemble the 1.44MB image --------------------------------------------
image: $(BUILD)/boot.bin $(BUILD)/loader.bin $(BUILD)/kernel.bin
	$(DD) if=/dev/zero of=$(BUILD)/novaos.img bs=512 count=2880 status=none
	$(DD) if=$(BUILD)/boot.bin   of=$(BUILD)/novaos.img conv=notrunc status=none
	$(DD) if=$(BUILD)/loader.bin of=$(BUILD)/novaos.img bs=512 seek=1 conv=notrunc status=none
	$(DD) if=$(BUILD)/kernel.bin of=$(BUILD)/novaos.img bs=512 seek=3 conv=notrunc status=none
	@echo "==> $(BUILD)/novaos.img ready"

check: image data
	@echo "--- image layout ---"
	@$(DD) if=$(BUILD)/novaos.img bs=1 count=2 skip=510 status=none | xxd | head -1
	@echo "boot signature: 55 aa (above)"
	@echo "--- data disk ---"
	@ls -lh $(BUILD)/data.img

run: image data
	qemu-system-i386 -fda $(BUILD)/novaos.img -fdb $(BUILD)/data.img -serial stdio

clean:
	rm -rf $(BUILD)

$(BUILD):
	mkdir -p $(BUILD)/kernel $(BUILD)/libc
