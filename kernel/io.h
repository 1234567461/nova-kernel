/* NovaOS - x86 port I/O primitives */
#ifndef NOVA_IO_H
#define NOVA_IO_H

#include "common.h"

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* enable interrupts / halt */
static inline void sti(void)  { __asm__ volatile ("sti"); }
static inline void cli(void)  { __asm__ volatile ("cli"); }
static inline void hlt(void)  { __asm__ volatile ("hlt"); }

#endif
