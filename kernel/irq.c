/* NovaOS - 8259 PIC driver, remaps IRQ0-15 to vectors 32-47 */
#include "irq.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(void) {
    /* cascade init sequence */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   /* master offset 32 */
    outb(PIC2_DATA, 0x28); io_wait();   /* slave  offset 40 */
    outb(PIC1_DATA, 0x04); io_wait();   /* slave at IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFF);              /* mask all */
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void irq_enable(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8  bit  = (irq < 8) ? irq : irq - 8;
    u8  mask = inb(port);
    outb(port, mask & ~(1 << bit));
}

void irq_disable(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8  bit  = (irq < 8) ? irq : irq - 8;
    u8  mask = inb(port);
    outb(port, mask | (1 << bit));
}
