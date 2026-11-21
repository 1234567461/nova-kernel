# ============================================================================
#  NovaOS bootloader - hard-disk stage 1 (written by the `install` wizard)
#
#  Variant of boot.asm for a disk installed by the NovaOS install wizard.
#  Differences from the floppy stage 1:
#    * loads stage2 + kernel from fixed LBAs at the TAIL of the disk, so the
#      FAT12 data area at the front stays usable after installation
#    * reads via INT 13h AH=42h (extended LBA read) - correct on hard disks
#      where we cannot assume the 18-sectors-per-track floppy geometry
#    * bytes 3..61 are a BPB hole: the installer copies them from the disk's
#      existing boot sector so the FAT12 volume stays mountable afterwards
# ============================================================================
    .code16
    .intel_syntax noprefix

    .section .text
    .globl _start

    .set LOADER_SEG,       0x0000
    .set LOADER_OFF,       0x7E00
    .set KERNEL_SEG,       0x1000
    .set KERNEL_OFF,       0x0000
    .set HD_LOADER_LBA,    2676      # loader lives at the tail of the disk
    .set HD_KERNEL_LBA,    2678
    .set HD_KERNEL_SECTORS, 200      # must match stage2 KERNEL_SIZE

_start:
    jmp short start
    nop
    # BPB hole - 59 bytes, overwritten by the installer with the on-disk BPB
    .space 59

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, offset msg_boot
    call print_string

    # --- loader: HD_LOADER_LBA, 2 sectors -> 0x7E00 -------------------
    mov dl, [boot_drive]
    mov ebx, HD_LOADER_LBA
    mov ax, LOADER_SEG
    mov es, ax
    mov di, LOADER_OFF
    mov cx, 2
    call read_lba

    # --- kernel: HD_KERNEL_LBA, 200 sectors -> 0x10000 ----------------
    mov dl, [boot_drive]
    mov ebx, HD_KERNEL_LBA
    mov ax, KERNEL_SEG
    mov es, ax
    mov di, KERNEL_OFF
    mov cx, HD_KERNEL_SECTORS
    call read_lba

    mov si, offset msg_ok
    call print_string

    push word ptr LOADER_SEG
    push word ptr LOADER_OFF
    retf

    # ===================================================================
    #  read_lba - cx sectors from LBA ebx into ES:DI via INT 13h AH=42h
    #  (one sector per call; advances the buffer across 64KB boundaries)
    # ===================================================================
read_lba:
    pusha
    mov [sect_left], cx
    mov [buf_off], di
    mov [buf_seg], es
    mov [cur_lba_lo], bx
    mov word ptr [cur_lba_hi], 0

.rl_loop:
    # patch the Disk Address Packet for this sector
    mov word ptr [dap_count], 1
    mov ax, [buf_off]
    mov [dap_off], ax
    mov ax, [buf_seg]
    mov [dap_seg], ax
    mov ax, [cur_lba_lo]
    mov [dap_lba], ax
    mov ax, [cur_lba_hi]
    mov [dap_lba+2], ax

    mov dl, [cur_drive]
    mov si, offset dap
    mov ah, 0x42
    int 0x13
    jc .rl_error

    add word ptr [buf_off], 512
    jnc .rl_nowrap
    add word ptr [buf_seg], 0x1000
.rl_nowrap:
    inc word ptr [cur_lba_lo]
    adc word ptr [cur_lba_hi], 0
    dec word ptr [sect_left]
    jnz .rl_loop

    popa
    clc
    ret

.rl_error:
    mov si, offset msg_disk_err
    call print_string
    popa
    stc
    ret

print_string:
    pusha
.l:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .l
.done:
    popa
    ret

# Disk Address Packet for AH=42h
dap:
    .byte 0x10              # size
    .byte 0                 # reserved
dap_count:
    .word 1                 # sectors to transfer
dap_off:
    .word 0                 # buffer offset
dap_seg:
    .word 0                 # buffer segment
dap_lba:
    .long 0                 # LBA (64-bit, low half used here)

boot_drive:   .byte 0x00
cur_drive:    .byte 0x00
sect_left:    .word 0x0000
buf_off:      .word 0x0000
buf_seg:      .word 0x0000
cur_lba_lo:   .word 0x0000
cur_lba_hi:   .word 0x0000

msg_boot:     .asciz "NovaOS HD boot v0.8\n"
msg_ok:       .asciz "kernel loaded\n"
msg_disk_err: .asciz "disk error\n"

    .org 510
    .word 0xAA55
