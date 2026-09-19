/* NovaOS - round-robin scheduler.
 * Context switch is done by saving/restoring ESP on each task's private
 * stack; the IRQ stub's iret returns into the resumed context.  User
 * tasks carry their own page directory (cr3) and switching between
 * address spaces reloads CR3 (v1.0: per-process page tables + COW).
 */
#include "sched.h"
#include "kheap.h"
#include "mm.h"
#include "gdt.h"
#include "user.h"
#include "paging.h"
#include "io.h"
#include "string.h"
#include "vga.h"
#include "serial.h"

static task_t tasks[MAX_TASKS];
static u32    task_count = 0;
static u32    current = 0;
static u32    next_pid = 1;
static int    sched_ready = 0;

/* set up an initial stack frame so the task "returns" into fn() */
static void task_setup_stack(task_t *t, task_fn fn) {
    u32 *sp = (u32*)(t->stack_bottom + TASK_STACK);
    /* fake iret frame: eip=fn, cs=0x08, eflags=0x202 (IF set) */
    *--sp = 0x202;
    *--sp = 0x08;
    *--sp = (u32)fn;
    /* the isr stub pops gs fs es ds + pushad before iret: push placeholders */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;       /* gs fs es ds */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;       /* edi esi ebp esp */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;       /* ebx edx ecx eax */
    *--sp = 0; *--sp = 0;                             /* int_no err_code */
    t->esp = (u32)sp;
}

/* ring-3 task: first iret returns with user CS/SS/ESP (in user window) */
static void task_setup_user_stack(task_t *t, u32 entry) {
    u32 area = (u32)kmalloc(0x1000);      /* private frame for first iret */
    if (!area) return;
    u32 *sp = (u32*)(area + 0x1000);
    *--sp = GDT_UDATA_SEL;                /* user ss  */
    *--sp = USER_STACK_TOP;               /* user esp */
    *--sp = 0x202;                        /* eflags: IF */
    *--sp = GDT_UCODE_SEL;                /* user cs  */
    *--sp = entry;                        /* eip      */
    *--sp = 0; *--sp = 0;                 /* int_no err_code */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* edi esi ebp esp */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebx edx ecx eax */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* gs fs es ds */
    t->esp = (u32)sp;
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));
    task_count = 0;
    current = 0;
    sched_ready = 0;
}

u32 task_create(const char *name, task_fn fn) {
    if (task_count >= MAX_TASKS) return 0;
    task_t *t = &tasks[task_count];
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_KERNEL;
    t->stack_bottom = (u32)kmalloc(TASK_STACK);
    if (!t->stack_bottom) return 0;
    memset((void*)t->stack_bottom, 0xCC, TASK_STACK);
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    task_setup_stack(t, fn);
    task_count++;
    return t->pid;
}

u32 task_create_user(const char *name, u32 entry, u32 cr3) {
    if (task_count >= MAX_TASKS) return 0;
    task_t *t = &tasks[task_count];
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_USER;
    t->stack_bottom = 0;              /* interrupts use the shared TSS stack */
    t->cr3 = cr3;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    task_setup_user_stack(t, entry);
    task_count++;
    return t->pid;
}

/* fork(2): clone the current user task.  The child gets a COW copy of
 * the parent's address space and resumes at the same instruction with
 * eax = 0 (the parent sees the child pid from the syscall return). */
u32 task_fork_user(const char *name, u32 parent_esp, u32 parent_cr3) {
    if (task_count >= MAX_TASKS) return 0;
    u32 child_cr3 = paging_fork_addr_space(parent_cr3);
    if (!child_cr3) return 0;
    task_t *t = &tasks[task_count];
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_USER;
    t->stack_bottom = 0;
    t->cr3 = child_cr3;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    /* child frame: same return context, eax slot = 0 */
    t->esp = parent_esp;
    *(u32*)(t->esp + 44) = 0;         /* pushad eax slot in the frame */
    task_count++;
    return t->pid;
}

static void switch_to(u32 next) {
    if (!sched_ready) { current = next; return; }
    u32 old = current;
    /* reload CR3 when entering a user task (or leaving one) */
    if (tasks[old].flags == TASK_USER || tasks[next].flags == TASK_USER) {
        u32 cr3 = (tasks[next].flags == TASK_USER) ? tasks[next].cr3 : KERNEL_PD;
        paging_switch(cr3);
    }
    __asm__ volatile (
        "mov %%esp, %0\n\t"
        "mov %1, %%esp\n\t"
        : "=m"(tasks[old].esp) : "r"(tasks[next].esp) : "memory");
    current = next;
}

void sched_yield(void) {
    if (task_count < 2) return;
    u32 next = (current + 1) % task_count;
    if (!tasks[next].state) { return; }   /* keep it simple */
    switch_to(next);
}

void sched_tick(void) {
    if (task_count < 2) return;
    if (tasks[current].slice_left > 0) {
        tasks[current].slice_left--;
        return;
    }
    /* time slice expired: find next ready task */
    for (u32 i = 1; i <= task_count; i++) {
        u32 next = (current + i) % task_count;
        if (tasks[next].state) {
            tasks[current].slice_left = 5;
            tasks[next].slice_left = 5;
            switch_to(next);
            return;
        }
    }
}

u32 sched_current_pid(void) { return tasks[current].pid; }

task_t *sched_current_task(void) { return &tasks[current]; }

u32 sched_task_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < task_count; i++)
        if (tasks[i].state) n++;
    return n;
}

/* called from sys_exit(): the current task is dead, switch to the next
 * ready task by unwinding its frame directly (never returns here). */
void sched_exit_current(void) {
    tasks[current].state = 0;
    /* release the exiting process's address space (COW refcounts drop,
     * private frames are freed) */
    if (tasks[current].flags == TASK_USER && tasks[current].cr3)
        paging_destroy_addr_space(tasks[current].cr3);
    for (u32 i = 1; i <= MAX_TASKS; i++) {
        u32 next = (current + i) % MAX_TASKS;
        if (tasks[next].state) {
            tasks[next].slice_left = 5;
            if (tasks[next].flags == TASK_USER)
                paging_switch(tasks[next].cr3);
            else
                paging_switch(KERNEL_PD);
            __asm__ volatile (
                "mov %0, %%esp\n\t"
                "pop %%gs\n\t"
                "pop %%fs\n\t"
                "pop %%es\n\t"
                "pop %%ds\n\t"
                "popal\n\t"
                "add $8, %%esp\n\t"
                "iret\n"
                : : "r"(tasks[next].esp) : "memory");
            for (;;) hlt();     /* unreachable */
        }
    }
    vga_write("\n[sys] all tasks exited - halting\n", 0x0C);
    cli();
    for (;;) hlt();
}
