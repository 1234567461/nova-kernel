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
    .set KERNEL_SIZE,    260*512     # must match stage1 sector count

_start:
    cli

    # --- VBE: ask the firmware for a true-colour linear framebuffer -----
    # This has to happen in real mode, before paging exists.  We probe VBE
    # 2.0+, walk the mode list and pick 1024x768x32 with a linear
    # framebuffer (attribute bit 7).  The result is written to a fixed
    # struct at 0x8F00 that the kernel reads; magic 0 means "no VBE" and
    # the kernel falls back to the old mode-13h path.
    #
    # The old build only ever used mode 13h (320x200x256): that is why the
    # desktop looked like 1993 no matter how the widgets were drawn.
    # --- enable A20 (fast gate) --------------------------------------
    # Must run before the VBE call: SeaVGABIOS walks memory above 1MB
    # while it builds the mode list, which needs A20 to be on.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    call vbe_setup

    # --- load GDT ------------------------------------------------------
    # Must use the 16-bit form explicitly: "lgdt [gdt_desc]" in .code16
    # makes GNU as emit a 32-bit operand form (66-prefixed) while the
    # R_386_16 relocation only patches 2 address bytes, so the instruction
    # ends up 3 bytes short and the following bytes decode as garbage.
    # lgdtw is the correct 16-bit-base descriptor-pointer load here.
    lgdtw [gdt_desc]

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

    # --- map the VBE linear framebuffer as a single 4MB page ------------
    # The kernel touches the LFB through its physical address (identity
    # map), so one 4MB page directory entry + CR4.PSE is all it needs.
    # If the firmware did not give us VBE, or the base is not 4MB aligned,
    # skip it and let the kernel fall back to mode 13h.
    mov eax, dword ptr [0x8F00]
    cmp eax, 0x31454256                # 'VBE1'
    jne .no_lfb
    mov eax, dword ptr [0x8F14]        # PhysBasePtr
    test eax, 0x3FFFFF
    jnz .no_lfb
    mov ebx, eax
    shr ebx, 22                        # PDE index
    or eax, 0x83                       # present | writable | 4MB page
    mov dword ptr [0x9000 + ebx*4], eax
    mov eax, cr4
    or eax, 0x10                       # CR4.PSE
    mov cr4, eax
.no_lfb:

    # --- enable paging + PE ---------------------------------------------
    mov eax, cr0
    or eax, 0x80000001              # PG | PE
    mov cr0, eax

    # --- jump to kernel --------------------------------------------------
    jmp 0x08:0x100000

    # ===================================================================
    #  vbe_setup - choose a 32bpp linear-framebuffer mode and program it
    #  (real mode only).  Result goes to the bootinfo struct at 0x8F00:
    #      0x8F00 u32 magic 'VBE1'   (0 = no VBE, kernel uses mode 13h)
    #      0x8F04 u32 width
    #      0x8F08 u32 height
    #      0x8F0C u32 pitch (bytes per scanline)
    #      0x8F10 u32 bpp
    #      0x8F14 u32 framebuffer physical address
    # ===================================================================
vbe_setup:
    pusha
    # stage1 leaves ES pointing at the kernel staging segment (0x1000), so
    # every "mov [0x8200], ..." below would land at 0x18200 instead.  The
    # VBE calls write through ES:DI and read absolute addresses via DS, so
    # both have to be flat before anything else happens.
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov word ptr [0xB8000], 0x1F56     # 'V': probe entered
    mov dword ptr [0x8F00], 0xEE000001 # stage 1: entered probe

    mov di, 0x8200                     # VbeInfoBlock (control info)
    mov ax, 0x4F00
    int 0x10
    mov word ptr [0xB8002], 0x1F41     # 'A': int 10h returned
    mov dword ptr [0x8F00], 0xEE000002 # stage 2: int 10h returned
    cmp ax, 0x004F
    jne .vs_out
    cmp dword ptr [0x8200], 0x41534556 # 'VESA'
    jne .vs_out
    mov word ptr [0xB8004], 0x1F42     # 'B': signature ok
    mov dword ptr [0x8F00], 0xEE000003 # stage 3: VBE signature ok

    mov bx, 1024
    mov cx, 768
    call vbe_find
    test ax, ax
    jnz .vs_got
    mov bx, 800
    mov cx, 600
    call vbe_find
    test ax, ax
    jnz .vs_got
    mov bx, 640
    mov cx, 480
    call vbe_find
    test ax, ax
    jz .vs_out
    mov word ptr [0xB8006], 0x1F43     # 'C': mode found
    mov dword ptr [0x8F00], 0xEE000004 # stage 4: mode found
.vs_got:
    or ax, 0x4000                      # bit 14: use linear framebuffer
    mov bx, ax
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vs_out

    # mode is live: publish its parameters
    mov dword ptr [0x8F00], 0x31454256 # 'VBE1'
    xor eax, eax
    mov ax, word ptr [0x8014]          # XResolution
    mov dword ptr [0x8F04], eax
    xor eax, eax
    mov ax, word ptr [0x8016]          # YResolution
    mov dword ptr [0x8F08], eax
    xor eax, eax
    mov ax, word ptr [0x8012]          # BytesPerScanLine
    mov dword ptr [0x8F0C], eax
    mov dword ptr [0x8F10], 32
    mov eax, dword ptr [0x802A]        # PhysBasePtr
    mov dword ptr [0x8F14], eax
.vs_out:
    popa
    ret

# -----------------------------------------------------------------------
#  vbe_find - scan the VBE mode list for bx x cx, 32bpp, LFB capable
#    in : bx = width, cx = height
#    out: ax = mode number (0 = not found)
# -----------------------------------------------------------------------
vbe_find:
    push bx
    push cx
    push dx
    push si
    push di
    push fs
    push bp

    mov bp, bx                         # bp = wanted width
    mov bx, cx                         # bx = wanted height (cx is needed
                                       #      by 4F01 for the mode number)
    mov si, word ptr [0x820E]          # VideoModePtr offset
    mov ax, word ptr [0x8210]          # VideoModePtr segment
    mov fs, ax
.vf_loop:
    mov dx, word ptr fs:[si]
    cmp dx, 0xFFFF
    je .vf_none
    add si, 2
    push si
    mov ax, 0x4F01
    mov cx, dx
    mov di, 0x8000
    int 0x10
    pop si
    cmp ax, 0x004F
    jne .vf_loop
    mov ax, word ptr [0x8000]          # ModeAttributes
    and ax, 0x0081                     # supported | linear framebuffer
    cmp ax, 0x0081
    jne .vf_loop
    cmp byte ptr [0x801B], 32          # BitsPerPixel
    jne .vf_loop
    cmp word ptr [0x8014], bp          # XResolution
    jne .vf_loop
    cmp word ptr [0x8016], bx          # YResolution
    jne .vf_loop
    mov ax, dx                         # found it
    jmp .vf_done
.vf_none:
    xor ax, ax
.vf_done:
    pop bp
    pop fs
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

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
