# ============================================================================
#  NovaOS bootloader - Stage 1 (512-byte boot sector)
#  Written from scratch in GNU as (Intel syntax). No GRUB, no filesystem.
#
#  Responsibilities:
#    1. Set up a flat 1.44MB "floppy" image loaded by the BIOS at 0x7C00
#    2. Load stage2 (loader.bin) from sectors 2..3  -> 0x7E00
#    3. Load kernel.bin  from sectors 4..N          -> 0x10000 (temporary)
#    4. Jump to stage2
#
#  Layout of the raw image produced by the Makefile:
#     sector 0          : this boot sector
#     sectors 1-2       : loader.bin  (<= 1024 bytes)
#     sectors 3..end    : kernel.bin
# ============================================================================

    .code16
    .intel_syntax noprefix

    .section .text
    .globl _start

    .set LOADER_SEG,  0x0000
    .set LOADER_OFF,  0x7E00          # stage2 target
    .set KERNEL_SEG,  0x1000          # 0x10000:0000 = 64KB mark
    .set KERNEL_OFF,  0x0000
    .set SECT_PER_TRACK, 18
    .set HEADS,         2
    .set LOADER_START_SECTOR, 2       # 1-based: loader begins at sector 2
    .set LOADER_SECTORS,   2          # loader fits in 2 sectors (1024B)
    .set KERNEL_START_SECTOR, 4       # kernel begins at sector 4

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    # --- save boot drive number (passed by BIOS in dl) ---
    mov [boot_drive], dl

    # --- print banner -------------------------------------------------
    mov si, offset msg_boot
    call print_string

    # --- load loader.bin: CHS read, sectors 2-3 -----------------------
    mov dl, [boot_drive]
    mov si, LOADER_START_SECTOR
    mov di, LOADER_SEG
    mov ax, LOADER_OFF
    mov cx, LOADER_SECTORS
    call read_sectors

    # --- load kernel.bin: CHS read from sector 4 ----------------------
    mov dl, [boot_drive]
    mov si, KERNEL_START_SECTOR
    mov di, KERNEL_SEG
    mov ax, KERNEL_OFF
    mov cx, 96                      # 96 sectors * 512 = 49152 bytes max kernel
    call read_sectors

    mov si, offset msg_ok
    call print_string

    # --- jump to stage2 ------------------------------------------------
    jmp LOADER_SEG:LOADER_OFF

    # ===================================================================
    #  read_sectors(dl=drive, si=LBA(1-based), di=seg, ax=offset, cx=count)
    #  Converts LBA to CHS:  sector = (LBA % 18)+1, head=(LBA/18)%2,
    #                        cyl = LBA / (18*2)
    # ===================================================================
read_sectors:
    pusha
    mov [dap_count], cx
    xor bx, bx                      # bx = LBA (0-based)
    mov bx, si
    dec bx                          # convert to 0-based
.loop:
    mov dx, 0
    mov ax, bx
    mov cx, SECT_PER_TRACK
    div cx                          # ax = head/cyl part, dx = sector(0-based)
    mov [chs_sector], dl
    inc byte ptr [chs_sector]       # sectors are 1-based
    mov dx, 0
    mov cx, HEADS
    div cx                          # ax = cyl, dx = head
    mov [chs_head], dl
    mov [chs_cyl], al

    mov ax, di
    mov es, ax
    mov bx, [save_offset]
    mov ah, 0x02                    # BIOS: read sectors
    mov al, 1
    mov ch, [chs_cyl]
    mov cl, [chs_sector]
    mov dh, [chs_head]
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

    # advance offset by 512 bytes
    add word ptr [save_offset], 512
    jnc .no_seg_advance
    mov ax, es
    add ax, 0x1000                  # carry across 64KB boundary
    mov es, ax
    mov di, ax
.no_seg_advance:
    inc bx
    dec word ptr [dap_count]
    jnz .loop
    popa
    ret

.disk_error:
    mov si, offset msg_disk_err
    call print_string
    jmp .halt
.halt:
    cli
    hlt
    jmp .halt

    # ===================================================================
    #  print_string(si) - BIOS teletype output
    # ===================================================================
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

    # -------------------------------------------------------------------
    # (single .text section so objcopy -O binary produces one flat blob)
boot_drive:    .byte 0x00
chs_sector:    .byte 0x00
chs_head:      .byte 0x00
chs_cyl:       .byte 0x00
dap_count:     .word 0x0000
save_offset:   .word 0x0000

msg_boot:      .asciz "NovaOS boot v0.1\n"
msg_ok:        .asciz "kernel loaded\n"
msg_disk_err:  .asciz "disk error\n"

    .org 510
    .word 0xAA55
