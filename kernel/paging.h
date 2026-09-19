/* NovaOS - paging: identity map of the whole 4MB region */
#ifndef NOVA_PAGING_H
#define NOVA_PAGING_H

#include "common.h"

void paging_init(void);
u32  paging_get_cr3(void);

#endif
