/* ============================================================================
 * NovaOS - kernel entry stub
 *
 * This is the *first* function in the kernel image (see KEEP(.text.entry)
 * in link.ld).  The stage-2 bootloader jumps straight to 0x100000 with
 * paging already enabled and an identity map over the first 4MB, so all we
 * have to do here is set up a clean stack, zero the BSS and call into C.
 *
 * Note on the linker symbols: kernel_stack_top / __bss_* are *absolute*
 * symbols produced by link.ld, not variables.  Declaring them as arrays of
 * unknown size and taking their address is the portable way to use them -
 * `extern u32 kernel_stack_top;` would emit a load from that address and
 * fault on the very first instruction.
 * ========================================================================== */
#include "common.h"

/* absolute symbols from link.ld */
extern char kernel_stack_top[];
extern char __bss_start[];
extern char __bss_end[];

void kernel_main(void);

/* --- very early serial output --------------------------------------------
 * kernel_main() prints through kprintf(), but if anything before that goes
 * wrong the machine is silent and there is no way to tell how far boot got.
 * These inline helpers talk straight to COM1 so the entry path can leave a
 * breadcrumb even before the UART driver is initialised.
 */
static inline void early_outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void early_serial_init(void) {
    early_outb(0x3F9, 0x00);      /* disable interrupts        */
    early_outb(0x3FB, 0x80);      /* enable DLAB               */
    early_outb(0x3F8, 0x03);      /* divisor low  = 3 (38400)  */
    early_outb(0x3F9, 0x00);      /* divisor high = 0          */
    early_outb(0x3FB, 0x03);      /* 8N1, DLAB off             */
    early_outb(0x3FA, 0xC7);      /* FIFO enable + clear       */
    early_outb(0x3FC, 0x0B);      /* IRQs on, RTS/DSR          */
}

static inline u8 early_inb(u16 port) {
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void early_puts(const char *s) {
    while (*s) {
        while (!(early_inb(0x3FD) & 0x20))
            ;                                       /* spin until THR free */
        early_outb(0x3F8, (u8)*s++);
    }
}

__attribute__((noreturn, section(".text.entry"), used))
void _start(void) {
    /* switch to the kernel stack carved out by the linker script */
    u32 sp = (u32)kernel_stack_top;
    __asm__ volatile ("mov %0, %%esp" : : "r"(sp));

    early_serial_init();
    early_puts("[entry] _start reached, bss 0x");
    /* a tiny hex dump so a silent hang is still diagnosable */
    {
        u32 v = (u32)__bss_end;
        for (int i = 28; i >= 0; i -= 4) {
            u8 nib = (u8)((v >> i) & 0xF);
            early_outb(0x3F8, (u8)(nib < 10 ? '0' + nib : 'a' + nib - 10));
        }
    }
    early_puts("\n");

    /* clear BSS - the loader only copied the raw image, .bss is not in it */
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;

    early_puts("[entry] bss cleared, jumping to kernel_main\n");

    kernel_main();

    early_puts("[entry] kernel_main returned - halting\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
}
