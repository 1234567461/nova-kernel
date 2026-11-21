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
    xor ax, ax
    mov ds, ax                       # stage1 may leave ds != 0

    # TRACE ch: one raw byte out to COM1.  The firmware has already
    # programmed the UART, so a single out is enough.  This is our only
    # reliable debug channel here: text-mode writes through [0xB8xxx]
    # get their 16-bit address truncated in .code16, and there is no
    # VGA escape hatch once the shell clears the screen.
    # TRACE ch: write one char into VGA text page 1 (0xB9000..).
    # The kernel never touches that page (mode 13h graphics live at
    # 0xA0000, text page 0 at 0xB8000), so a post-mortem read of
    # 0xB9000 via QEMU's monitor shows exactly how far the loader got.
    # Serial/debugcon both proved unreliable here, so this is our
    # ground-truth debug channel.
    .macro TRACE ch
    push ax
    push bx
    push es
    mov ax, 0xB900
    mov es, ax
    mov bx, word ptr [trace_pos]
    mov al, \ch
    mov ah, 0x0F
    mov word ptr es:[bx], ax
    add bx, 2
    mov word ptr [trace_pos], bx
    pop es
    pop bx
    pop ax
    .endm

    # VBW reg val: write a word into the bochs/QEMU VBE register file
    # (index port 0x1CE, data port 0x1CF).  This programs the extended
    # graphics mode without any BIOS help - same as the Linux
    # bochs-drm driver does.
    .macro VBW reg val
    mov dx, 0x1CE
    mov al, \reg
    out dx, al
    mov dx, 0x1CF
    mov ax, \val
    out dx, ax
    .endm


    TRACE 'L'                          # stage2 entered

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

    # VBE BIOS calls need interrupts enabled: the SeaVGABIOS handlers
    # tick-wait internally and simply never return while IF=0.  But the
    # QEMU IDE controller keeps raising IRQ14 for the data disk, and the
    # BIOS IDE driver never acks it, so enabling IF unmasked causes an
    # interrupt storm that corrupts every BIOS call in flight.  Mask the
    # ATA lines first.
    in al, 0xA1
    or al, 0xC0                        # IRQ14 | IRQ15
    out 0xA1, al
    sti
    TRACE 'V'                          # entering vbe_setup

    # call vbe_setup -- written by hand.  In .intel_syntax + .code16 GNU as
    # still emits a *32-bit* E8 call (4-byte rel) for `call vbe_setup`,
    # which desynchronises the instruction stream the moment it executes.
    # We emit the 16-bit call ourselves: E8 + rel16, where rel16 counts
    # from the address of the next instruction (i.e. the current position
    # of `.` plus the 2 bytes the .word below occupies).
    .byte 0xE8
    .word vbe_setup - (. + 2)

    # interrupts off from here on: the BIOS IVT is about to become
    # meaningless in protected mode
    cli

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

    # --- back to 16-bit code --------------------------------------------
    # vbe_setup/vbe_find run in REAL MODE and must be assembled as 16-bit
    # code.  Inside a .code32 region as encodes every ax/bx operation with
    # the 66h operand-size prefix ("16-bit op inside 32-bit code"), but the
    # CPU executes them in 16-bit mode where 66h means the opposite, so
    # each instruction eats two extra bytes and the stream desynchronises.
    .code16

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
    # every "mov [0x9A00], ..." below would land at 0x18200 instead.  The
    # VBE calls write through ES:DI and read absolute addresses via DS, so
    # both have to be flat before anything else happens.
    xor ax, ax
    mov ds, ax
    mov es, ax

    TRACE '1'                          # entering probe

    mov di, 0x9A00                     # VbeInfoBlock (control info)
    mov ax, 0x4F00
    int 0x10
    TRACE '2'                          # 4F00 ok
    cmp ax, 0x004F
    jne .vs_out
    # 'VESA' signature check.  Do NOT write "cmp dword ptr [mem], imm32"
    # in .code16: as emits the 16-bit C7/81 form and the 4-byte immediate
    # desynchronises the stream.  Go through a register instead, which
    # makes as emit the 66h operand-size prefix.
    mov eax, dword ptr [0x9A00]        # 'VESA'
    cmp eax, 0x41534556
    jne .vs_out
    TRACE '3'                          # signature ok
    # debug: copy VideoModePtr + the first 8 mode words to 0x8300
    mov si, word ptr [0x9A0E]          # VideoModePtr offset
    mov ax, word ptr [0x9A10]          # VideoModePtr segment
    mov fs, ax
    mov di, 0x8300
    mov cx, 8
.dbg_copy:
    mov ax, word ptr fs:[si]
    mov word ptr [di], ax
    add si, 2
    add di, 2
    loop .dbg_copy

    # Direct VESA standard mode numbers instead of walking the mode list:
    # SeaVGABIOS's 4F01 handler wedges on some planar-mode queries, so we
    # just try the standard linear-framebuffer modes with 4F02 and read
    # the parameters back afterwards.
    # SeaVGABIOS's 4F02 only knows modes 0x100-0x107 (it returns 0x014F
    # for anything else), but its 4F01 happily reports the VESA standard
    # modes.  So: query parameters with 4F01, then program the bochs/QEMU
    # VBE register file directly - exactly what the Linux bochs-drm
    # driver does, no BIOS mode-set support required.
    TRACE 'T'
    mov cx, 0x11C                      # 1024x768x32 (SeaVGABIOS 32bpp modes)
    call vbe_params
    test ax, ax
    jz .prog
    mov cx, 0x118                      # 800x600x32
    call vbe_params
    test ax, ax
    jz .prog
    mov cx, 0x114                      # 640x480x32
    call vbe_params
    test ax, ax
    jnz .vs_out
.prog:
    TRACE 'P'                          # parameters in hand
    # the scan found SeaVGABIOS's own mode number (e.g. 0x144 for
    # 1024x768x32) - program it through the BIOS so every register
    # sequence is exactly what the firmware intends
    mov bx, word ptr [0x8F26]          # best mode from the scan
    or bx, 0x4000                      # use the linear framebuffer
    mov ax, 0x4F02
    int 0x10
    mov word ptr [0x8320], ax          # debug: 4F02 rc
    cmp ax, 0x004F
    jne .vs_out
    TRACE 'E'                          # mode live
    # publish the parameters read from the 4F01 buffer.
    # NOTE: "mov dword ptr [mem], imm32" is broken in .code16 (as emits
    # the 16-bit C7 form and the stream desynchronises).  Every 32-bit
    # store goes through a register so as emits the 66h prefix.
    mov eax, 0x31454256                # 'VBE1'
    mov dword ptr [0x8F00], eax
    # NB: `mov ax, [mem]` does not clear the high half of eax - zero it
    # first or the stale 0x3145 ('VE') ends up in the published dword.
    xor eax, eax
    mov ax, word ptr [0x9C12]          # XResolution
    mov dword ptr [0x8F04], eax
    xor eax, eax
    mov ax, word ptr [0x9C14]          # YResolution
    mov dword ptr [0x8F08], eax
    xor eax, eax
    mov ax, word ptr [0x9C10]          # BytesPerScanLine
    mov dword ptr [0x8F0C], eax
    mov eax, 32                        # bpp
    mov dword ptr [0x8F10], eax
    mov eax, dword ptr [0x9C28]        # PhysBasePtr
    mov dword ptr [0x8F14], eax
.vs_out:
    popa
    ret

# -----------------------------------------------------------------------
#  vbe_find - scan the VBE mode list for bx x cx, 32bpp, LFB capable
#    in : bx = width, cx = height
#    out: ax = mode number (0 = not found)
# -----------------------------------------------------------------------
# -----------------------------------------------------------------------
#  vbe_params - 4F01 the mode in CX into the buffer at 0x9C00 and verify
#  it is a 32bpp linear-framebuffer mode.
#    in : cx = VESA mode number
#    out: ax = 0 on success (buffer filled), 1 on failure
# -----------------------------------------------------------------------
vbe_params:
    # walk the mode list reported by 4F00 and take the first mode that is
    # supported, LFB-capable and 32bpp, preferring 1024x768 over 800x600
    # and 640x480.  The planar modes 0x102/0x104/0x106 are blacklisted:
    # the SeaVGABIOS 4F01 handler never returns for them in our setup.
    push si
    push di
    mov si, word ptr [0x9A0E]          # VideoModePtr offset
    mov ax, word ptr [0x9A10]          # VideoModePtr segment
    mov fs, ax
.scan:
    mov dx, word ptr fs:[si]
    cmp dx, 0xFFFF
    je .scan_none
    add si, 2
    cmp dx, 0x0102                     # blacklist planar wedge modes
    je .scan
    cmp dx, 0x0104
    je .scan
    cmp dx, 0x0106
    je .scan
    push si
    mov ax, 0x4F01
    mov cx, dx
    mov di, 0x9C00
    int 0x10
    pop si
    # the BIOS clobbers FS; reload the mode-list segment every iteration
    push ax
    mov ax, word ptr [0x9A10]
    mov fs, ax
    pop ax
    cmp ax, 0x004F
    jne .scan
    mov ax, word ptr [0x9C00]          # ModeAttributes
    and ax, 0x0081                     # supported | linear framebuffer
    cmp ax, 0x0081
    jne .scan
    cmp byte ptr [0x9C19], 32          # BitsPerPixel (MIB+0x19)
    jne .scan
    cmp word ptr [0x9C12], 1024        # prefer 1024x768, else keep scanning
    je .scan_hit_w
    cmp word ptr [0x9C12], 800
    je .scan_hit_maybe
    cmp word ptr [0x9C12], 640
    jne .scan
    mov bx, 1                          # score 1: 640x480
    jmp .scan_maybe
.scan_hit_w:
    mov bx, 3                          # score 3: 1024x768
    jmp .scan_maybe
.scan_hit_maybe:
    mov bx, 2                          # score 2: 800x600
.scan_maybe:
    mov ax, dx
    mov word ptr [0x8F20], ax          # remember current candidate
    mov word ptr [0x8F22], bx          # score
    # keep looking for something better, but remember this one
    mov ax, word ptr [0x8F24]          # best score so far (0 = none)
    cmp bx, ax
    jbe .scan
    mov ax, dx
    mov word ptr [0x8F26], ax          # best mode so far
    mov word ptr [0x8F24], bx
    jmp .scan
.scan_none:
    mov ax, word ptr [0x8F26]          # best mode found (0 = none)
    test ax, ax
    jz .scan_fail
    mov cx, ax                         # re-query the winner into the buffer
    mov ax, 0x4F01
    mov di, 0x9C00
    int 0x10
    push ax
    mov ax, word ptr [0x9A10]
    mov fs, ax
    pop ax
    xor ax, ax                         # success (buffer holds the params)
    pop di
    pop si
    ret
.scan_fail:
    mov ax, 1
    pop di
    pop si
    ret

trace_pos:
    .word 0

gdt_start:
    .quad 0x0000000000000000        # null descriptor
    .quad 0x00CF9A000000FFFF        # code: base 0, limit 4GB, DPL0, exec/read
    .quad 0x00CF92000000FFFF        # data: base 0, limit 4GB, DPL0, read/write
gdt_end:

gdt_desc:
    .word gdt_end - gdt_start - 1
    .long gdt_start
