/* NovaOS - syscall dispatch (int 0x80) */
#ifndef NOVA_SYSCALL_H
#define NOVA_SYSCALL_H

#include "common.h"
#include "idt.h"

void syscall_dispatch(struct regs *r);

#endif
