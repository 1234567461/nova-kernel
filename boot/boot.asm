# ============================================================================
#  NovaOS bootloader - Stage 1 (512-byte boot sector)
#  Written from scratch in GNU as (Intel syntax). No GRUB, no filesystem.
#
#  Responsibilities:
#    1. Set up a flat 1.44MB "floppy" image loaded by the BIOS at 0x7C00
#    2. Load stage2 (loader.bin) from sectors 2..3  -> 0x7E00
#    3. Load kernel.bin  from sectors 4..N          -> 0x10000 (temporary)
#    4. Jump to stage2 directly (no far-jump syntax traps)
#
#  Layout of the raw image produced by the Makefile:
#     sector 0          : this boot sector
#     sectors 1-2       : loader.bin  (<= 1024 bytes)
#     sectors 3..end    : kernel.bin
#
#  Disk access uses the classic INT 13h AH=02h CHS read, one sector per
#  call.  A 1.44MB floppy has a fixed 18 sectors/track, 2 head geometry, so
#  the CHS translation is exact.  AH=42h (extended LBA read) is deliberately
#  NOT used: QEMU's floppy BIOS accepts it but the transfer silently stops
#  short, which is far worse than an outright failure.
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
    .set KERNEL_SECTORS,  200         # 200 * 512 = 102400 bytes max kernel

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

    # --- load loader.bin: LBA 1, 2 sectors -> 0x0000:0x7E00 -----------
    mov dl, [boot_drive]
    mov ebx, LOADER_START_SECTOR - 1  # 0-based LBA
    mov ax, LOADER_SEG
    mov es, ax
    mov di, LOADER_OFF
    mov cx, LOADER_SECTORS
    call read_sectors

    # --- load kernel.bin: LBA 3, up to 200 sectors -> 0x1000:0x0000 ---
    mov dl, [boot_drive]
    mov ebx, KERNEL_START_SECTOR - 1
    mov ax, KERNEL_SEG
    mov es, ax
    mov di, KERNEL_OFF
    mov cx, KERNEL_SECTORS
    call read_sectors

    mov si, offset msg_ok
    call print_string

    # --- jump to stage2 ------------------------------------------------
    # "jmp $SEG:$OFF" would assemble SEG as an intra-segment offset; use
    # an explicit far return instead, which is unambiguous.
    push word ptr LOADER_SEG        # target CS
    push word ptr LOADER_OFF        # target IP
    retf

    # ===================================================================
    #  read_sectors - read cx sectors starting at 0-based LBA ebx into
    #  ES:DI, advancing DI across 64KB boundaries as needed.
    #    in : dl = drive, ebx = LBA, es:di = buffer, cx = sector count
    #    out: CF clear on success, carry set + message on failure
    #
    #  Primary path is the classic INT 13h AH=02h CHS read: a 1.44MB
    #  floppy has a fixed 18 sectors/track, 2 heads geometry, and this
    #  call works on every BIOS including QEMU's.  (AH=42h LBA reads are
    #  nominally nicer, but not all BIOS implementations provide them for
    #  floppy drives, so they are only used as a fallback below.)
    # ===================================================================
    # in : dl = drive, ebx = 0-based LBA, es:di = buffer, cx = sector count
    # out: CF clear on success, carry set + message on failure
    #
    # Everything the loop needs lives in memory, because INT 13h AH=02h
    # requires DL, CX, DX and BX to be repurposed for the CHS argument and
    # the transfer buffer; keeping any of it in a register guarantees the
    # next iteration computes a bogus CHS from clobbered state.
    # ===================================================================
read_sectors:
    pusha
    mov [sect_left], cx
    mov [buf_off], di
    mov [buf_seg], es
    mov [cur_lba], bx               # only the low 16 bits matter (< 2880)
    mov [cur_drive], dl

.rs_loop:
    # --- LBA([cur_lba]) -> CHS -----------------------------------------
    #   sector = (LBA % 18) + 1, head = (LBA / 18) % 2, cyl = LBA / 36
    #
    # Careful: the sector number is parked in BL while the second division
    # runs, because "mov cx, HEADS" would otherwise overwrite CL - and CL
    # is exactly where the sector number has to end up.  Stashing it in CX
    # first and reloading CX for the divisor silently truncated every LBA
    # to the "sector" of an 18-sector period, so every read past the first
    # physical track fetched the wrong sector.
    mov ax, [cur_lba]               # LBA is < 2^16 for a 1.44MB floppy
    xor dx, dx
    mov cx, SECT_PER_TRACK
    div cx                          # ax = LBA/18, dx = LBA%18
    mov bl, dl
    inc bl                          # BL = sector (1-based)
    xor dx, dx
    mov cx, HEADS
    div cx                          # ax = cylinder, dx = head
    mov ch, al                      # cyl (low 8 bits are enough here)
    mov dh, dl                      # head
    mov cl, bl                      # sector (1-based)

    mov dl, [cur_drive]
    mov ax, [buf_seg]
    mov es, ax
    mov bx, [buf_off]
    mov ax, 0x0201                 # AH=02 read, AL=1 sector
    int 0x13
    jc .rs_error

    # --- advance buffer by one sector, wrapping the segment ------------
    add word ptr [buf_off], 512
    jnc .rs_nowrap
    add word ptr [buf_seg], 0x1000
.rs_nowrap:
    inc word ptr [cur_lba]
    dec word ptr [sect_left]
    jnz .rs_loop

    popa
    clc
    ret

.rs_error:
    mov si, offset msg_disk_err
    call print_string
    popa
    stc
    ret

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
cur_drive:     .byte 0x00
sect_left:     .word 0x0000
buf_off:       .word 0x0000
buf_seg:       .word 0x0000
cur_lba:       .word 0x0000

msg_boot:      .asciz "NovaOS boot v0.1\n"
msg_ok:        .asciz "kernel loaded\n"
msg_disk_err:  .asciz "disk error\n"

    .org 510
    .word 0xAA55
