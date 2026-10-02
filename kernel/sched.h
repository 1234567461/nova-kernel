/* NovaOS - round-robin scheduler (kernel threads + ring-3 processes) */
#ifndef NOVA_SCHED_H
#define NOVA_SCHED_H

#include "common.h"
#include "idt.h"

#define MAX_TASKS    16
#define TASK_STACK   0x4000         /* 16KB per kernel-thread stack */
/* Per-task kernel stack used for ring-3 -> ring-0 entries (TSS.esp0) and for
 * the interrupt frames the scheduler saves.  It must be private to the task:
 * a single shared stack makes every task's parked frame live at the same
 * address, so a resumed task reads a stranger's context. */
#define IRQ_STACK    0x2000         /* 8KB per task */
/* an interrupt frame (struct regs) is 5 + 8 + 2 + 5 = 20 dwords; give fork's
 * private frame copy room for that plus the pushad/stub slack */
#define FRAME_BYTES  0x100          /* 256 B: comfortably > sizeof(struct regs) */

#define TASK_KERNEL  0              /* kernel thread (ring 0) */
#define TASK_USER    1              /* user process (ring 3) */

typedef void (*task_fn)(void);

typedef struct task {
    u32  pid;
    int  state;                     /* 1 = ready, 0 = dead */
    u32  flags;                     /* TASK_KERNEL / TASK_USER */
    u32  esp;                       /* saved stack pointer */
    u32  stack_bottom;              /* kernel-thread stack (from kmalloc) */
    u32  irq_stack_top;             /* private ring0 stack for interrupts */
    u32  cr3;                       /* own page directory (user tasks) */
    u32  slice_left;                /* ticks remaining in time slice */
    char name[16];
} task_t;

void sched_init(void);
u32  sched_adopt_current(const char *name);  /* make the running context task 0 */
u32  task_create(const char *name, task_fn fn);
u32  task_create_user(const char *name, u32 entry, u32 cr3);
u32  task_fork_user(const char *name, u32 parent_frame, u32 parent_cr3);
void sched_enter_irq(struct regs *r);   /* stub tells us the live irq frame */
void sched_yield(void);             /* cooperative yield */
void sched_tick(void);              /* called by timer IRQ */
void sched_exit_current(void);      /* never returns - switches away */
u32  sched_current_pid(void);
task_t *sched_current_task(void);
u32  sched_task_count(void);

/* read-only views of the task table for the shell's `ps` command */
u32  sched_task_pid(u32 slot);
u32  sched_task_state(u32 slot);
const char *sched_task_name(u32 slot);

#endif
