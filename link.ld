/* ============================================================================
 * NovaOS kernel linker script
 * Kernel is loaded at 1MB by the stage-2 bootloader, paging identity-maps
 * the first 4MB.  We produce a flat binary (kernel.bin) via objcopy.
 * ========================================================================== */

ENTRY(kernel_main)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4K)
    {
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

    end = .;
}
