/* NovaOS - PS/2 keyboard driver (8042) */
#ifndef NOVA_KEYBOARD_H
#define NOVA_KEYBOARD_H

#include "common.h"

void    keyboard_init(void);
/* returns 0 when the ring buffer is empty */
int     keyboard_getc(void);
int     keyboard_pending(void);

#endif
