/* NovaOS - user program linker script.
 * Produces a single-segment ET_EXEC loaded in the user window
 * (0x200000..0x2FFFFF).  All sections are discarded/merged so the
 * kernel's minimal ELF loader sees exactly one PT_LOAD segment.
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    . = 0x200000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
        *(.sdata*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
        *(.init*)
        *(.fini*)
        *(.got*)
        *(.plt*)
    }
}
