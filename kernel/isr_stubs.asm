# ============================================================================
#  NovaOS - interrupt service routine stubs
#  Each stub pushes (in reverse order) a dummy error code when the CPU does
#  not provide one, then pushes the interrupt number and calls the common
#  C handler.  IRQ 0-15 are remapped to vectors 32-47 by the PIC.
# ============================================================================

    .intel_syntax noprefix
    .section .text

    .macro ISR_NOERR n
    .globl isr\n
isr\n:
    push 0
    push \n
    jmp isr_common
    .endm

    .macro ISR_ERR n
    .globl isr\n
isr\n:
    push \n
    jmp isr_common
    .endm

    .set IRQ_BASE, 32

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR   8
    ISR_NOERR 9
    ISR_ERR   10
    ISR_ERR   11
    ISR_ERR   12
    ISR_ERR   13
    ISR_ERR   14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_NOERR 17
    ISR_NOERR 18
    ISR_NOERR 19
    ISR_NOERR 20
    ISR_NOERR 21
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_NOERR 29
    ISR_NOERR 30
    ISR_NOERR 31

    /* IRQ 0..15 -> vectors 32..47 */
    ISR_NOERR 32
    ISR_NOERR 33
    ISR_NOERR 34
    ISR_NOERR 35
    ISR_NOERR 36
    ISR_NOERR 37
    ISR_NOERR 38
    ISR_NOERR 39
    ISR_NOERR 40
    ISR_NOERR 41
    ISR_NOERR 42
    ISR_NOERR 43
    ISR_NOERR 44
    ISR_NOERR 45
    ISR_NOERR 46
    ISR_NOERR 47

    /* system call vector 128 (0x80) */
    ISR_NOERR 128

isr_common:
    # push the remaining registers in the order expected by struct regs
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    # stack now: gs fs es ds edi esi ebp esp ebx edx ecx eax int_no err
    mov eax, isr_common_handler
    call eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                  # drop int_no + err_code
    iret
