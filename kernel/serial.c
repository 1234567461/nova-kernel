/* NovaOS - 16550 UART driver */
#include "serial.h"
#include "io.h"

static int serial_ready_tx(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);          /* disable interrupts */
    outb(COM1 + 3, 0x80);          /* enable DLAB */
    outb(COM1 + 0, 0x03);          /* divisor low  = 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);          /* divisor high = 0 */
    outb(COM1 + 3, 0x03);          /* 8N1, no DLAB */
    outb(COM1 + 2, 0xC7);          /* FIFO enable, clear, 14-byte */
    outb(COM1 + 4, 0x0B);          /* IRQs enabled, RTS/DSR set */
}

void serial_putc(char c) {
    for (int i = 0; i < 100000 && !serial_ready_tx(); i++) ;
    outb(COM1, (u8)c);
}

void serial_write(const char *s, u32 len) {
    for (u32 i = 0; i < len; i++) serial_putc(s[i]);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

/* Non-blocking receive.
 *
 * Returns the next byte from the UART RX FIFO, or -1 when nothing has
 * arrived.  The shell polls this alongside the PS/2 keyboard so the machine
 * can be driven from a serial console (QEMU -serial stdio/pipe/socket) with
 * no display at all - which is also how it is tested in CI. */
int serial_getc(void) {
    if (!(inb(COM1 + 5) & 0x01)) return -1;      /* LSR bit0: data ready */
    return (int)(u8)inb(COM1);
}
