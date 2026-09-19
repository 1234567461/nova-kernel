/* NovaOS - PIC (8259) driver */
#ifndef NOVA_IRQ_H
#define NOVA_IRQ_H

#include "common.h"

void pic_init(void);
void pic_eoi(u8 irq);
void irq_enable(u8 irq);
void irq_disable(u8 irq);

#endif
