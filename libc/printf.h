/* NovaOS libc - minimal vsnprintf/printf for kernel and userland */
#ifndef NOVA_PRINTF_H
#define NOVA_PRINTF_H

#include "common.h"
#include <stdarg.h>

int kvsnprintf(char *buf, u32 size, const char *fmt, va_list args);
int ksprintf(char *buf, const char *fmt, ...);
void kprintf(const char *fmt, ...);

#endif
