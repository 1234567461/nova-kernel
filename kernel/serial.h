/* NovaOS - 16550 UART driver (COM1 debug console) */
#ifndef NOVA_SERIAL_H
#define NOVA_SERIAL_H

#include "common.h"

#define COM1 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, u32 len);
void serial_puts(const char *s);

#endif
