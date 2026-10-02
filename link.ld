/* ============================================================================
 * NovaOS kernel linker script
 * Kernel is loaded at 1MB by the stage-2 bootloader, paging identity-maps
 * the first 4MB.  We produce a flat binary (kernel.bin) via objcopy.
 *
 * IMPORTANT: objcopy -O binary throws the ELF header (and with it the entry
 * point) away, so the stage-2 loader simply jumps to 0x100000.  The entry
 * routine therefore MUST be the very first thing in the image - that is what
 * the KEEP(.text.entry) clause below guarantees.  Without it the linker puts
 * whichever object file happens to come first at 0x100000 and the CPU starts
 * executing a random function's prologue.
 * ========================================================================== */

ENTRY(kernel_main)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4K)
    {
        KEEP(*(.text.entry))       /* kernel entry stub - must be first */
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    /* Kernel stack: 16KB, top of the symbol is the initial ESP */
    . = ALIGN(4K);
    . = . + 0x4000;
    kernel_stack_top = .;

    /* System-call / interrupt stack: used as the ring-0 stack when a
     * ring-3 process is interrupted (TSS.esp0 points here). */
    . = ALIGN(4K);
    . = . + 0x2000;
    sys_stack_top = .;

    end = .;
}
