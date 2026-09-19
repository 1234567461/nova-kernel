/* NovaOS - common type definitions and kernel-wide constants */
#ifndef NOVA_COMMON_H
#define NOVA_COMMON_H

typedef unsigned char   u8;
typedef unsigned short  u16;
typedef unsigned int    u32;
typedef signed char     i8;
typedef signed short    i16;
typedef signed int      i32;

#define NULL ((void*)0)
#define TRUE  1
#define FALSE 0

#define KERNEL_BASE 0x100000u
#define MEM_END     0x400000u        /* 4MB identity-mapped region */

#endif
