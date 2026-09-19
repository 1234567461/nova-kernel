# ============================================================================
#  NovaOS bootloader - Stage 2
#  Runs at 0x7E00 in real mode, loaded by stage1.
#
#  Responsibilities:
#    1. Enable A20 gate (fast A20 via port 0x92)
#    2. Load a flat GDT (code + data, base 0, limit 4GB)
#    3. Switch to 32-bit protected mode
#    4. Copy kernel.bin from its staging location to 0x100000 (1MB)
#    5. Set up a minimal page table (identity map the kernel) and
#       enable paging
#    6. Jump to kernel entry at 0x100000
# ============================================================================

    .code16
    .intel_syntax noprefix

    .section .text
    .globl _start

    .set KERNEL_STAGING, 0x10000     # where stage1 put the kernel
    .set KERNEL_DEST,    0x100000    # final load address (1MB)
    .set KERNEL_SIZE,    200*512     # must match stage1 sector count

_start:
    cli

    # --- enable A20 (fast gate) --------------------------------------
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    # --- load GDT ------------------------------------------------------
    lgdt [gdt_desc]

    # --- switch to protected mode --------------------------------------
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    # flush prefetch queue
    jmp 0x08:pm_entry

    .code32
pm_entry:
    # --- reload segment registers --------------------------------------
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    # --- copy kernel from 0x10000 to 0x100000 --------------------------
    cld
    mov esi, KERNEL_STAGING
    mov edi, KERNEL_DEST
    mov ecx, KERNEL_SIZE / 4
    rep movsd

    # --- enable paging: identity map 0..4MB (two page tables) ----------
    # Page Directory at 0x9000, first page table at 0xA000
    mov eax, 0x9000
    mov cr3, eax

    # zero the page directory (1024 entries)
    mov edi, eax
    xor eax, eax
    mov ecx, 1024
    rep stosd

    # zero the first page table
    mov edi, 0xA000
    xor eax, eax
    mov ecx, 1024
    rep stosd

    # map first 4MB: 1024 pages, 4KB each, present + writable
    mov edi, 0xA000
    mov eax, 0x00000003             # addr 0, present, writable
    mov ecx, 1024
.map_pt:
    stosd
    add eax, 0x1000
    loop .map_pt

    # PDE[0] -> page table at 0xA000 (present, writable)
    mov eax, 0xA000
    or eax, 0x00000003
    mov [0x9000], eax

    # --- enable paging + PE ---------------------------------------------
    mov eax, cr0
    or eax, 0x80000001              # PG | PE
    mov cr0, eax

    # --- jump to kernel --------------------------------------------------
    jmp 0x08:0x100000

    # ===================================================================
    #  GDT: null, code (base0, 4GB, exec/read), data (base0, 4GB, rw)
    #  (kept in .text so the flat binary layout is predictable)
    # ===================================================================
    .align 8
gdt_start:
    .quad 0x0000000000000000        # null descriptor
    .quad 0x00CF9A000000FFFF        # code: base 0, limit 4GB, DPL0, exec/read
    .quad 0x00CF92000000FFFF        # data: base 0, limit 4GB, DPL0, read/write
gdt_end:

gdt_desc:
    .word gdt_end - gdt_start - 1
    .long gdt_start
