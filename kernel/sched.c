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
#include "printf.h"
#include "irq.h"

static task_t tasks[MAX_TASKS];
static u32    task_count = 0;
static u32    current = 0;
static u32    next_pid = 1;
static int    sched_ready = 0;

/* pointer to the struct regs of the interrupt currently being handled; it is
 * the save area used when a preemptive context switch happens.
 *
 * NOTE: this must NOT be used to save the outgoing context.  The frame lives on
 * the *interrupt* stack, which is re-entered by the very next IRQ (and by every
 * other task), so a pointer to it goes stale the moment we switch away.  The
 * outgoing stack pointer is taken from the running task's saved ESP instead
 * (see sched_enter_irq), which is the only value that survives a switch.
 */
static struct regs *sched_current_frame = NULL;

/* True once an IRQ frame has been observed: only from then on is the
 * saved-ESP-in-task model consistent and preemption safe. */
void sched_enter_irq(struct regs *r) {
    /* First IRQ after boot: pin the adopted boot thread to the live frame so
     * its ESP is a real interrupt stack address from the start. */
    if (!sched_ready && task_count > 0 && tasks[current].esp == 0)
        tasks[current].esp = (u32)r;
    sched_current_frame = r;
    sched_ready = 1;
}

/* Adopt the context that is executing right now (the kernel main thread) as a
 * schedulable task.  Without this, `current` names tasks[0] while the CPU is
 * actually running the boot thread, so the first switch saves/restores the
 * wrong stack and no demo task ever makes progress. */
u32 sched_adopt_current(const char *name) {
    if (task_count >= MAX_TASKS) return 0;
    task_t *t = &tasks[task_count];
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_KERNEL;
    t->stack_bottom = 0;
    t->cr3 = 0;
    t->slice_left = 5;
    t->esp = 0;                      /* filled in by sched_enter_irq on 1st IRQ */
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    /* The adopted context is the boot thread: it is already running on the
     * linker-provided kernel stack and has been taking interrupts there since
     * boot.  Keep esp0 exactly as it is (0 => "do not touch the TSS") so the
     * stack it has been using all along stays valid. */
    current = task_count;
    task_count++;
    return t->pid;
}

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

/* Find a free task slot.  Dead slots below task_count are reused before the
 * table is extended, so process churn (fork/exit in a loop) never exhausts
 * MAX_TASKS.  Returns the slot index, or MAX_TASKS when the table is full. */
static u32 alloc_slot(void) {
    for (u32 i = 0; i < task_count; i++)
        if (!tasks[i].state) return i;
    if (task_count < MAX_TASKS) return task_count++;
    return MAX_TASKS;
}

/* Give a task its own ring0 interrupt stack.  Returns 0 on failure.  Called
 * for every schedulable slot, including the adopted boot thread. */
static int alloc_irq_stack(task_t *t) {
    if (t->irq_stack_top) return 1;
    u32 base = (u32)kmalloc(IRQ_STACK);
    if (!base) return 0;
    t->irq_stack_top = base + IRQ_STACK;     /* stacks grow down */
    return 1;
}

u32 task_create(const char *name, task_fn fn) {
    u32 slot = alloc_slot();
    if (slot >= MAX_TASKS) return 0;
    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_KERNEL;
    t->stack_bottom = (u32)kmalloc(TASK_STACK);
    if (!t->stack_bottom) { t->state = 0; return 0; }
    if (!alloc_irq_stack(t)) { t->state = 0; return 0; }
    memset((void*)t->stack_bottom, 0xCC, TASK_STACK);
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    task_setup_stack(t, fn);
    return t->pid;
}

u32 task_create_user(const char *name, u32 entry, u32 cr3) {
    u32 slot = alloc_slot();
    if (slot >= MAX_TASKS) return 0;
    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_USER;
    t->stack_bottom = 0;
    if (!alloc_irq_stack(t)) { t->state = 0; return 0; }
    t->cr3 = cr3;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    task_setup_user_stack(t, entry);
    return t->pid;
}

/* fork(2): clone the current user task.  The child gets a COW copy of
 * the parent's address space and resumes at the same instruction with
 * eax = 0 (the parent sees the child pid from the syscall return).
 *
 * `parent_frame` points at the *live syscall frame* on the parent's private
 * kernel stack.  The child is scheduled later, and before it runs the parent
 * (and everyone else) will take more interrupts; even though each task now
 * owns a stack, the child has none of its own yet, so resuming straight from
 * the parent's frame would read whatever the parent's stack holds by then.
 *
 * The frame is therefore *copied* into the child's own pending-frame slot,
 * which nothing else can touch.  It is released implicitly with the task slot
 * when the child exits and that slot is reused.
 */
u32 task_fork_user(const char *name, u32 parent_frame, u32 parent_cr3) {
    u32 slot = alloc_slot();
    if (slot >= MAX_TASKS) return 0;
    u32 child_cr3 = paging_fork_addr_space(parent_cr3);
    if (!child_cr3) return 0;

    u32 frame_copy = (u32)kmalloc(FRAME_BYTES);
    if (!frame_copy) {
        paging_destroy_addr_space(child_cr3);
        return 0;
    }
    memcpy((void*)frame_copy, (void*)parent_frame, FRAME_BYTES);

    task_t *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->state = 1;
    t->flags = TASK_USER;
    t->stack_bottom = 0;
    if (!alloc_irq_stack(t)) {
        t->state = 0;
        kfree((void*)frame_copy);
        paging_destroy_addr_space(child_cr3);
        return 0;
    }
    t->cr3 = child_cr3;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->slice_left = 5;
    /* child frame: same return context, eax slot = 0 so the child sees
     * "I am the child" from fork()'s return value */
    t->esp = frame_copy;
    *(u32*)(t->esp + 44) = 0;         /* pushad eax slot in the frame */
    return t->pid;
}

/* Switch to task `next` from inside an interrupt handler.
 *
 * This is *not* a normal function call: after loading the new task's ESP we
 * must unwind the interrupt frame ourselves (pop gs/fs/es/ds, popad, drop
 * int_no/err_code, iret) instead of returning with `ret`.  A plain `ret`
 * would pop the first dword of the new task's frame - its interrupt number -
 * and jump there, which is exactly the triple-fault this used to cause.
 *
 * `frame` points at the struct regs the stub built for the *outgoing* task,
 * so that value is what we store as its saved ESP; when it is resumed later
 * the same unwind sequence continues from there.
 */
/* Parked address spaces.
 *
 * An exiting process must not free its own page directory while it is still
 * executing on it: the moment the frames are released, the very code and stack
 * running inside the exit path stop being mapped and the CPU faults.  The cr3
 * is parked here and released once the next task is up and running on its own
 * address space (see sched_exit_current).
 */
static u32 zombie_cr3 = 0;


/* Release a parked address space.  Only ever called from the process-exit
 * path (never from an IRQ handler): see the note in switch_to(). */
static void reap_zombies(void) {
    if (zombie_cr3) {
        paging_destroy_addr_space(zombie_cr3);
        zombie_cr3 = 0;
    }
}

static void switch_to(struct regs *frame, u32 next) {
    u32 old = current;

    /* Defensive: a bad slot index would index past tasks[] and fault deep in
     * the switch, where the cause is very hard to see.  Refuse to switch and
     * keep running the current task instead. */
    if (next >= MAX_TASKS || old >= MAX_TASKS || !tasks[next].state) {
        serial_write("[sched] switch_to: bad slot, staying put\n", 42);
        return;
    }

    /* The IRQ has already been acknowledged by isr_common_handler before it
     * called the handler, so a context switch that never returns there does
     * not leave the 8259 waiting for an EOI - and we must NOT send a second
     * one, which would rotate the PIC priority chain. */

    /* Enter a critical section: the state we are about to publish (cr3, esp0,
     * current) must be consistent before any further interrupt can observe
     * it.  The iret below restores the incoming task's own IF bit, so
     * interrupts come back on exactly when that task resumes. */
    cli();

    /* reload CR3 when entering a user task (or leaving one) */
    if (tasks[old].flags == TASK_USER || tasks[next].flags == TASK_USER) {
        u32 cr3 = (tasks[next].flags == TASK_USER) ? tasks[next].cr3 : KERNEL_PD;
        paging_switch(cr3);
    }

    /* Save the *live* interrupt frame as the outgoing context.  `frame` is a
     * pointer into the interrupt stack and is therefore the correct address to
     * remember: when this task is resumed we reload ESP with it and unwind the
     * exact same frame, continuing right where the IRQ interrupted us.
     *
     * The frame still lives on the kernel interrupt stack (the stub's pushad +
     * iret frame), which is untouched while we run another task, so the address
     * stays valid across the switch. */
    tasks[old].esp = (u32)frame;

    /* Hand the CPU the incoming task's own ring0 stack before we jump into it.
     * From now on every interrupt it takes builds its frame on that stack, so
     * the address we just saved above stays unique to the task we are leaving
     * and cannot be stomped by whoever runs next. */
    if (tasks[next].irq_stack_top)
        user_set_kernel_stack(tasks[next].irq_stack_top);

    current = next;

    /* NOTE: do not reap parked address spaces here.
     *
     * This runs in interrupt context, on an interrupt stack, with CR3 already
     * pointing at the incoming task.  Tearing down a page directory from here
     * means walk-and-free work on the physical allocator while the stack we
     * are standing on is not the one the free expects, and it showed up as a
     * general-protection fault at the instruction right after the free call.
     * Reaping is deferred to sched_exit_current(), which runs on the exiting
     * process's own kernel stack outside the IRQ path. */

    __asm__ volatile (
        "mov %0, %%esp\n\t"
        "pop %%gs\n\t"
        "pop %%fs\n\t"
        "pop %%es\n\t"
        "pop %%ds\n\t"
        "popal\n\t"
        "add $8, %%esp\n\t"
        "iret\n\t"
        : : "r"(tasks[next].esp) : "memory");
    for (;;) hlt();                    /* not reached */
}

void sched_yield(void) {
    if (task_count < 2) return;
    u32 next = (current + 1) % task_count;
    if (!tasks[next].state) return;
    struct regs *frame = sched_current_frame;
    if (!frame) return;                /* only valid from interrupt context */
    if (tasks[current].esp == 0) tasks[current].esp = (u32)frame;
    switch_to(frame, next);
}

void sched_tick(void) {
    if (task_count < 2) return;
    if (current >= task_count) return;      /* stale index: table shrank */
    if (!sched_ready) return;
    if (tasks[current].slice_left > 0) {
        tasks[current].slice_left--;
        return;
    }
    /* time slice expired: find next ready task */
    for (u32 i = 1; i <= task_count; i++) {
        u32 next = (current + i) % task_count;
        if (next >= task_count) continue;
        if (tasks[next].state) {
            struct regs *frame = sched_current_frame;
            if (!frame) return;
            tasks[current].esp = (u32)frame;
            tasks[current].slice_left = 5;
            tasks[next].slice_left = 5;
            switch_to(frame, next);
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

u32 sched_task_pid(u32 slot) {
    return slot < task_count ? tasks[slot].pid : 0;
}

u32 sched_task_state(u32 slot) {
    return slot < task_count ? (u32)tasks[slot].state : 0;
}

const char *sched_task_name(u32 slot) {
    return slot < task_count ? tasks[slot].name : "?";
}

/* called from sys_exit(): the current task is dead, switch to the next
 * ready task by unwinding its frame directly (never returns here). */
void sched_exit_current(void) {
    u32 me = current;

    /* The interrupt that brought us here (int 0x80, or a timer tick) was
     * already acknowledged by isr_common_handler before it dispatched, so
     * there is nothing to do for the PIC on this path. */

    /* Park our own address space for the incoming task to free; releasing it
     * here would unmap the code and stack we are still standing on. */
    if (tasks[me].flags == TASK_USER && tasks[me].cr3) {
        zombie_cr3 = tasks[me].cr3;
        tasks[me].cr3 = 0;
    }
    tasks[me].state = 0;
    { char d[72]; int l = ksprintf(d, "[sched] task pid=%u exited, n=%u\n",
        tasks[me].pid, task_count);
      serial_write(d, (u32)l); }

    /* pick the next runnable task (never ourselves) and jump into it */
    for (u32 i = 1; i < task_count; i++) {
        u32 next = (me + i) % task_count;
        if (next == me) continue;
        if (tasks[next].state) {
            tasks[next].slice_left = 5;

            /* Enter a critical section: from here to the iret we are mid-way
             * through adopting another context.  A timer tick landing in the
             * middle would see the half-updated state (current already moved,
             * esp0 still the old task's, or vice versa) and corrupt the
             * incoming task's saved esp by parking an IRQ frame in it. */
            cli();

            if (tasks[next].flags == TASK_USER)
                paging_switch(tasks[next].cr3);
            else
                paging_switch(KERNEL_PD);

            /* The incoming task's interrupts must land on its own ring0 stack
             * (see the note in switch_to).  Set before adopting the slot, so
             * the first IRQ after the iret already has the right esp0. */
            if (tasks[next].irq_stack_top)
                user_set_kernel_stack(tasks[next].irq_stack_top);

            /* Tear the departing address space down before the switch, and
             * before `current` is repointed, so nothing on the way to the iret
             * can observe a task slot that no longer owns a valid cr3.  Safe
             * here because this runs on the kernel's own stack, which the
             * kernel page directory maps in every address space. */
            reap_zombies();

            /* Adopt the slot we are jumping into last of all.  `current` is
             * what the next timer tick uses to find the running task; leaving
             * it on the dead slot made sched_tick() save the incoming task's
             * frame into that slot and then resume it from a stale fork frame,
             * so the process silently restarted at its fork point forever. */
            current = next;

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
