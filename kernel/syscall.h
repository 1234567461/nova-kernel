/* NovaOS - syscall dispatch (int 0x80) */
#ifndef NOVA_SYSCALL_H
#define NOVA_SYSCALL_H

#include "common.h"
#include "idt.h"

void syscall_dispatch(struct regs *r);
void files_flush_all(void);      /* write back all dirty open files */

#endif
