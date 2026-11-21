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
KERNEL_MAX := 133120

C_SRCS := kernel/entry.c kernel/gdt.c kernel/idt.c kernel/isr.c kernel/irq.c \
          kernel/timer.c kernel/keyboard.c kernel/mouse.c kernel/vga.c \
          kernel/serial.c kernel/mm.c kernel/kheap.c kernel/paging.c \
          kernel/sched.c kernel/syscall.c kernel/user.c kernel/ata.c \
          kernel/fat.c kernel/gui.c kernel/shell.c kernel/bootlog.c \
          kernel/pci.c kernel/ne2k.c kernel/rtl8139.c kernel/net.c kernel/update.c \
          kernel/rtc.c kernel/pkg.c kernel/vbe.c kernel/kernel.c \
          libc/string.c libc/printf.c
C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
ASM_OBJS := $(BUILD)/kernel/isr_stubs.o $(BUILD)/kernel/embed.o
K_OBJS := $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run check data efi iso run-uefi

all: image data

# --- boot sector + stage2 -----------------------------------------------
# Both blobs contain "offset symbol" references that GNU as leaves as
# R_386_16 relocations.  objcopy -O binary does NOT resolve them, so the
# linker has to - otherwise every data label reference is off by a few
# bytes and the boot sector reads garbage / jumps nowhere.
$(BUILD)/boot.bin: boot/boot.asm | $(BUILD)
	$(AS) --32 $< -o $(BUILD)/boot.o
	$(LD) -m elf_i386 -Ttext 0x7C00 --oformat binary -o $@ $(BUILD)/boot.o
	@test $$(stat -c%s $@) -eq 512 || (echo "ERROR: boot.bin != 512 bytes"; exit 1)
	@echo "boot.bin: $$(stat -c%s $@) bytes (sector 0)"

$(BUILD)/loader.bin: boot/loader.asm | $(BUILD)
	$(AS) --32 $< -o $(BUILD)/loader.o
	# The loader runs at 0x7E00, so every absolute reference inside it
	# (notably lgdt [gdt_desc]) must be relocated to that address.
	# Without this, lgdt reads from physical 0x00B8 and the CPU triple
	# faults the moment it tries to load CS with the bogus GDT.
	$(LD) -m elf_i386 -Ttext 0x7E00 --oformat binary -o $@ $(BUILD)/loader.o
	@test $$(stat -c%s $@) -le 4096 || (echo "ERROR: loader.bin > 4096 bytes"; exit 1)
	@echo "loader.bin: $$(stat -c%s $@) bytes (sectors 1-8)"

# --- hard-disk stage 1 (written to LBA0 by the `install` wizard) ---------
$(BUILD)/boothd.bin: boot/boothd.asm | $(BUILD)
	$(AS) --32 $< -o $(BUILD)/boothd.o
	$(LD) -m elf_i386 -Ttext 0x7C00 --oformat binary -o $@ $(BUILD)/boothd.o
	@test $$(stat -c%s $@) -eq 512 || (echo "ERROR: boothd.bin != 512 bytes"; exit 1)
	@echo "boothd.bin: $$(stat -c%s $@) bytes (HDD boot sector)"

# --- kernel ---------------------------------------------------------------
$(C_OBJS): $(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/isr_stubs.o: kernel/isr_stubs.asm | $(BUILD)
	$(AS) --32 $< -o $@

# boot-chain blobs (boothd.bin + loader.bin) embedded into the kernel so
# the `install` wizard can write a bootable hard disk without the floppy
$(BUILD)/kernel/embed.o: boot/embed.S $(BUILD)/boothd.bin $(BUILD)/loader.bin | $(BUILD)
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

# writer.elf: second ring-3 program, exercises the file *write* syscalls
$(BUILD)/userspace/writer.elf: userspace/writer.c userspace/user.ld | $(BUILD)
	mkdir -p $(BUILD)/userspace
	$(CC) -m32 -nostdlib -fno-pie -fno-stack-protector -fno-builtin \
	      -mno-sse -mno-mmx -mno-80387 -static \
	      -Wl,-T,userspace/user.ld -Wl,--build-id=none -o $@ $<
	@echo "writer.elf: user program ready"

# leaky.elf: ring-3 regression test - dirty file must survive exit without
# close(), and read-past-EOF must return 0 rather than leak kernel memory
$(BUILD)/userspace/leaky.elf: userspace/leaky.c userspace/user.ld | $(BUILD)
	mkdir -p $(BUILD)/userspace
	$(CC) -m32 -nostdlib -fno-pie -fno-stack-protector -fno-builtin \
	      -mno-sse -mno-mmx -mno-80387 -static \
	      -Wl,-T,userspace/user.ld -Wl,--build-id=none -o $@ $<
	@echo "leaky.elf: user program ready"

# --- FAT12 data disk (attached as B: in QEMU) ---------------------------
data: $(BUILD)/data.img

$(BUILD)/data.img: $(BUILD)/userspace/hello.elf $(BUILD)/userspace/writer.elf $(BUILD)/userspace/leaky.elf tools/mkdata.py | $(BUILD)
	python3 tools/mkdata.py $(BUILD)/data.img $(BUILD)/userspace/hello.elf \
	                         $(BUILD)/userspace/writer.elf \
	                         $(BUILD)/userspace/leaky.elf

# --- assemble the 1.44MB image --------------------------------------------
image: $(BUILD)/boot.bin $(BUILD)/loader.bin $(BUILD)/kernel.bin
	$(DD) if=/dev/zero of=$(BUILD)/novaos.img bs=512 count=2880 status=none
	$(DD) if=$(BUILD)/boot.bin   of=$(BUILD)/novaos.img conv=notrunc status=none
	$(DD) if=$(BUILD)/loader.bin of=$(BUILD)/novaos.img bs=512 seek=1 conv=notrunc status=none
	$(DD) if=$(BUILD)/kernel.bin of=$(BUILD)/novaos.img bs=512 seek=9 conv=notrunc status=none
	@echo "==> $(BUILD)/novaos.img ready"

check: image data
	@echo "--- image layout ---"
	@$(DD) if=$(BUILD)/novaos.img bs=1 count=2 skip=510 status=none | xxd | head -1
	@echo "boot signature: 55 aa (above)"
	@echo "--- data disk ---"
	@ls -lh $(BUILD)/data.img

run: image data
	qemu-system-i386 -fda $(BUILD)/novaos.img \
	    -drive file=$(BUILD)/data.img,format=raw,if=ide,index=2 \
	    -boot a -serial stdio

clean:
	rm -rf $(BUILD)

$(BUILD):
	mkdir -p $(BUILD)/kernel $(BUILD)/libc $(BUILD)/efi

# ===========================================================================
#  Native UEFI boot (x64).  Produces:
#    build/BOOTX64.EFI   - EFI application (PE32+), the UEFI "bootloader"
#    build/efiboot.img   - 4MB FAT image holding it + kernel.bin, used as the
#                          El Torito "-e" boot entry so the same ISO boots on
#                          both BIOS (NOVAOS.IMG) and UEFI (efiboot.img)
#  Kernel expectations (link.ld / paging.c) are unchanged: entry at 0x100000,
#  page directory 0x9000, page table 0xA000, identity map 0..4MB.  The EFI
#  loader (boot/efi/) reproduces the stage-2 environment before jumping in.
# ===========================================================================
EFICC   := gcc
EFICFLAGS := -Wall -Wextra -I/usr/include/efi -I/usr/include/efi/x86_64 \
             -DEFI_FUNCTION_WRAPPER -fno-stack-protector -fno-strict-aliasing \
             -fno-pic -fno-PIE -mno-red-zone -mcmodel=large -fpic \
             -fshort-wchar -O2 -g
EFILDS  := /usr/lib/elf_x86_64_efi.lds
EFICRT  := /usr/lib/crt0-efi-x86_64.o
EFILIBS := -L/usr/lib -lgnuefi -lefi

$(BUILD)/efi/main.o: boot/efi/efimain.c | $(BUILD)
	@mkdir -p $(BUILD)/efi
	$(EFICC) $(EFICFLAGS) -c $< -o $@

$(BUILD)/efi/switch.o: boot/efi/switch.S | $(BUILD)
	$(EFICC) -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(BUILD)/efi/main.o $(BUILD)/efi/switch.o
	$(LD) -T $(EFILDS) -shared -Bsymbolic \
	    -o $(BUILD)/efi/app.so $(EFICRT) $^ $(EFILIBS)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .rela.dyn -j .reloc \
	    --target=efi-app-x86_64 $(BUILD)/efi/app.so $@
	# objcopy does NOT translate ELF .rela.dyn into a PE .reloc table; without
	# it firmware relocates our image and every GOT slot stays a raw offset
	# -> #PF (RIP=0) right after the banner.  Patch it in ourselves.
	python3 tools/efi/pe-reloc.py $(BUILD)/efi/app.so $@
	@echo "BOOTX64.EFI: $$(stat -c%s $@) bytes"

# 4MB FAT12 image, layout: /EFI/BOOT/BOOTX64.EFI + /EFI/BOOT/KERNEL.BIN
$(BUILD)/efiboot.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin image
	mformat -C -i $@ -T 8192 ::
	mmd    -i $@ ::/EFI ::/EFI/BOOT
	mcopy  -i $@ $(BUILD)/BOOTX64.EFI    ::/EFI/BOOT/BOOTX64.EFI
	mcopy  -i $@ $(BUILD)/kernel.bin     ::/EFI/BOOT/KERNEL.BIN
	@echo "efiboot.img: UEFI boot image ready"

efi: $(BUILD)/efiboot.img

# dual-boot ISO.
#
# BIOS entry: ISOLINUX (El Torito no-emulation, 2KB load + self-reads) which
#   then loads our floppy image through memdisk.  This is the same boot path
#   Nova-Linux uses, and it is what every BIOS / VM firmware actually
#   implements correctly.  The previous layout pointed the El Torito BIOS
#   entry straight at NOVAOS.IMG, which xorriso turns into *floppy emulation*
#   (fd1.4); plenty of firmwares (and Android VM apps) cannot read that and
#   bail out with "Boot failed: Could not read from CDROM (code 0005)".
#
# UEFI entry: efiboot.img (El Torito "-e", no-emulation).
$(BUILD)/iso-src/NOVAOS.IMG: $(BUILD)/novaos.img $(BUILD)/efiboot.img | $(BUILD)
	mkdir -p $(BUILD)/iso-src/EFI/BOOT $(BUILD)/iso-src/isolinux
	cp $(BUILD)/novaos.img $(BUILD)/iso-src/NOVAOS.IMG
	cp $(BUILD)/efiboot.img $(BUILD)/iso-src/efiboot.img
	cp $(BUILD)/BOOTX64.EFI $(BUILD)/iso-src/EFI/BOOT/BOOTX64.EFI
	cp $(BUILD)/kernel.bin  $(BUILD)/iso-src/EFI/BOOT/KERNEL.BIN
	[ -f $(BUILD)/data.img ] && cp $(BUILD)/data.img $(BUILD)/iso-src/DATA.IMG || true
	cp /usr/lib/ISOLINUX/isolinux.bin $(BUILD)/iso-src/isolinux/
	cp /usr/lib/syslinux/modules/bios/ldlinux.c32 $(BUILD)/iso-src/isolinux/
	cp /usr/lib/syslinux/memdisk $(BUILD)/iso-src/isolinux/
	printf 'DEFAULT novaos\nPROMPT 0\nTIMEOUT 10\nLABEL novaos\n  KERNEL memdisk\n  APPEND initrd=/NOVAOS.IMG\n' \
	    > $(BUILD)/iso-src/isolinux/isolinux.cfg

iso: $(BUILD)/efiboot.img $(BUILD)/iso-src/NOVAOS.IMG
	xorriso -as mkisofs -V NOVAOS -R -J \
	    -b isolinux/isolinux.bin -c isolinux/boot.cat \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -eltorito-alt-boot \
	    -e efiboot.img   -no-emul-boot \
	    -o $(BUILD)/novaos-uefi.iso $(BUILD)/iso-src
	@echo "==> $(BUILD)/novaos-uefi.iso ready (BIOS isolinux+memdisk / UEFI)"

run-uefi: iso
	qemu-system-x86_64 -m 64 \
	    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
	    -drive file=$(BUILD)/data.img,format=raw,if=ide,index=2 \
	    -cdrom $(BUILD)/novaos-uefi.iso -serial stdio
