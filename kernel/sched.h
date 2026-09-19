/* NovaOS - round-robin scheduler (kernel threads) */
#ifndef NOVA_SCHED_H
#define NOVA_SCHED_H

#include "common.h"

#define MAX_TASKS    16
#define TASK_STACK   0x4000         /* 16KB per task stack */

typedef void (*task_fn)(void);

typedef struct task {
    u32  pid;
    int  state;                     /* 1 = ready, 0 = dead */
    u32  esp;                       /* saved stack pointer */
    u32  stack_bottom;              /* stack region base (from kmalloc) */
    u32  slice_left;                /* ticks remaining in time slice */
    char name[16];
} task_t;

void sched_init(void);
u32  task_create(const char *name, task_fn fn);
void sched_yield(void);             /* cooperative yield */
void sched_tick(void);              /* called by timer IRQ */
u32  sched_current_pid(void);
u32  sched_task_count(void);

#endif
