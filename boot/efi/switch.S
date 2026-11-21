# ============================================================================
#  NovaOS UEFI loader - long mode -> 32-bit protected mode trampoline
#
#  Called from efimain.c AFTER ExitBootServices() with:
#    rdi = kernel entry (0x100000)
#    rsi = page directory (0x9000)
#
#  The firmware brought us up in 64-bit long mode with its own page tables.
#  The kernel is a flat 32-bit binary that expects exactly what the BIOS
#  stage-2 sets up: flat 32-bit protected mode, 32-bit paging identity-
#  mapping the first 4MB, CR3 = 0x9000.  The switch order matters:
#
#    1. ljmp into a 32-bit code segment (compat mode, LMA still set)
#    2. clear CR0.PG          (LMA = LME && PG goes false -> legacy mode)
#    3. clear EFER.LME        (MSR 0xC0000080 bit 8)
#    4. clear CR4.PAE         (32-bit paging, no PAE - matches stage-2)
#    5. load CR3 = 0x9000
#    6. set CR0.PG            (now plain 32-bit paging)
#    7. reload data segments, jump to the kernel entry
# ============================================================================

    .text
    .code64
    .globl efi_go32
    .hidden efi_go32          # keep the call site direct (no GOT indirection)
    .type efi_go32, @function
efi_go32:
    cli
    lea  gdt_desc(%rip), %rax
    lgdt (%rax)

    # far jump into the 32-bit compat code segment (GDT entry 1).
    # In 64-bit mode the immediate form of ljmp (opcode EA) is invalid, so
    # we do it the Linux-way: push CS + target RIP and lretq.
    pushq $0x08
    leaq pm_entry(%rip), %rax
    pushq %rax
    lretq

    .code32
pm_entry:
    # --- paging off ---------------------------------------------------
    mov %cr0, %eax
    and  $0x7FFFFFFF, %eax          # clear PG
    mov  %eax, %cr0

    # --- EFER.LME off --------------------------------------------------
    mov  $0xC0000080, %ecx          # EFER MSR
    rdmsr
    and  $0xFFFFFEFF, %eax          # clear LME (bit 8)
    wrmsr

    # --- PAE off --------------------------------------------------------
    mov  %cr4, %eax
    and  $0xFFFFFFDF, %eax          # clear PAE (bit 5)
    mov  %eax, %cr4

    # --- stage-2 identity map live ---------------------------------------
    mov  %esi, %cr3                 # page directory at 0x9000

    # --- paging on again, now in plain 32-bit mode ----------------------
    mov  %cr0, %eax
    or   $0x80000000, %eax          # set PG
    mov  %eax, %cr0

    # CS already holds the 32-bit compat code selector from the first far
    # jump, so no second far jump is needed (an immediate ljmp here would
    # also emit an R_X86_64_32 relocation that -shared rejects).  Fall
    # through with near jumps only.
    mov  $0x10, %ax                 # flat data segments
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %fs
    mov  %ax, %gs
    mov  %ax, %ss
    mov  $0x70000, %esp             # temp stack; entry.c re-assigns ESP
    xor  %ebp, %ebp

    jmp  *%edi                      # kernel entry at 0x100000

    # ------------------------------------------------------------------
    #  GDT used only for the transition: null, 32-bit code, flat data.
    #  The kernel replaces it with its own in gdt_init().
    # ------------------------------------------------------------------
    .align 8
gdt:
    .quad 0x0000000000000000        # null
    .quad 0x00CF9A000000FFFF        # 32-bit code: base 0, limit 4GB
    .quad 0x00CF92000000FFFF        # flat data:   base 0, limit 4GB
gdt_end:

gdt_desc:
    .word gdt_end - gdt - 1
    .quad gdt
