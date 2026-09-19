/* NovaOS - round-robin scheduler (kernel threads) */
#ifndef NOVA_SCHED_H
#define NOVA_SCHED_H

#include "common.h"

#define MAX_TASKS    16
#define TASK_STACK   0x4000         /* 16KB per kernel-task stack */

#define TASK_KERNEL  0              /* kernel thread (ring 0) */
#define TASK_USER    1              /* user process (ring 3) */

typedef void (*task_fn)(void);

typedef struct task {
    u32  pid;
    int  state;                     /* 1 = ready, 0 = dead */
    u32  flags;                     /* TASK_KERNEL / TASK_USER */
    u32  esp;                       /* saved stack pointer */
    u32  stack_bottom;              /* stack region base (from kmalloc) */
    u32  slice_left;                /* ticks remaining in time slice */
    char name[16];
} task_t;

void sched_init(void);
u32  task_create(const char *name, task_fn fn);
u32  task_create_user(const char *name, u32 entry);   /* ring-3 process */
void sched_yield(void);             /* cooperative yield */
void sched_tick(void);              /* called by timer IRQ */
void sched_exit_current(void);      /* never returns - switches away */
u32  sched_current_pid(void);
u32  sched_task_count(void);

#endif
