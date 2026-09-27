#include <stdint.h>
#include "scheduler.h"
#include "task.h"
#include "console.h"
#include "tss.h"
#include "paging.h"

extern void timer_handler(void);

static uint32_t current_task_id = 0;

static void save_task_context(
    task_t *task,
    uint32_t *frame
)
{
    if (task == 0 || frame == 0)
        return;

    task->switch_esp = (uint32_t)frame;

    task->context.edi = frame[0];
    task->context.esi = frame[1];
    task->context.ebp = frame[2];

    task->context.esp =
        (uint32_t)frame + 32;

    task->context.ebx = frame[4];
    task->context.edx = frame[5];
    task->context.ecx = frame[6];
    task->context.eax = frame[7];

    task->context.eip = frame[8];
    task->context.eflags = frame[10];
}

static void scheduler_display(uint32_t id)
{
    volatile uint16_t *vga =
        (volatile uint16_t *)0xB8000;

    const char text[] = "SCHED:T00";

    uint32_t base = (2 * 80) + 68;

    for (uint32_t i = 0; i < 9; i++) {
        vga[base + i] =
            ((uint16_t)0x07 << 8) | text[i];
    }

    if (id < 10) {
        vga[base + 8] =
            ((uint16_t)0x07 << 8) |
            (uint8_t)('0' + id);
    }
}

/*
 * Point TSS.esp0 and CR3 at the task that is about to become
 * current. esp0 matters only for tasks that can take a
 * privilege-changing interrupt (Ring 3 tasks); for kernel-ring
 * tasks the value is simply unused by hardware. CR3 matters for
 * every task with its own address space.
 */
static void scheduler_activate(task_t *task)
{
    if (task == 0)
        return;

    tss_set_esp0(
        task->stack_base + task->stack_size
    );

    paging_switch_directory(
        task->page_directory
    );
}

void scheduler_init(void)
{
    current_task_id = 0;
    console_info("Scheduler initialized: OK");
}

uint32_t scheduler_current_task(void)
{
    return current_task_id;
}

uint32_t scheduler_next_task(void)
{
    /*
     * Round-robin search after current task. Ring 3 tasks are
     * now first-class schedulable entities, same as kernel-ring
     * ones.
     */
    for (uint32_t offset = 1;
         offset <= MAX_TASKS + 1;
         offset++) {

        uint32_t id =
            (current_task_id + offset) %
            (MAX_TASKS + 1);

        const task_t *task = task_get(id);

        if (task == 0)
            continue;

        if (task->state != TASK_READY)
            continue;

        if (task->switch_esp == 0)
            continue;

        return id;
    }

    /*
     * If no other task is ready, keep running current task.
     */
    return current_task_id;
}

void scheduler_tick(void)
{
    uint32_t next = scheduler_next_task();

    if (next == current_task_id)
        return;

    task_t *current =
        (task_t *)task_get(current_task_id);

    task_t *selected =
        (task_t *)task_get(next);

    if (current != 0 &&
        current->state == TASK_RUNNING) {

        current->state = TASK_READY;
    }

    if (selected != 0 &&
        selected->state == TASK_READY) {

        selected->state = TASK_RUNNING;
        current_task_id = next;

        scheduler_activate(selected);
        scheduler_display(next);
    }
}

uint32_t scheduler_irq(uint32_t *frame)
{
    timer_handler();

    task_t *current =
        (task_t *)task_get(current_task_id);

    if (current != 0) {
        /*
         * If the interrupted task is still active,
         * save its exact interrupt frame.
         */
        if (current->state == TASK_RUNNING ||
            current->state == TASK_READY) {

            save_task_context(current, frame);
        }
    }

    uint32_t next = scheduler_next_task();

    if (next == current_task_id) {

        /*
         * A current task may have yielded, blocked, or
         * terminated. If it is no longer runnable, try
         * to recover by selecting task 0.
         */
        if (current != 0 &&
            current->state != TASK_RUNNING) {

            const task_t *idle = task_get(0);

            if (idle != 0) {
                task_t *mutable_idle =
                    (task_t *)idle;

                mutable_idle->state = TASK_RUNNING;
                current_task_id = 0;

                scheduler_activate(mutable_idle);

                return (uint32_t)frame;
            }
        }

        return (uint32_t)frame;
    }

    task_t *selected =
        (task_t *)task_get(next);

    if (selected == 0 ||
        selected->switch_esp == 0)
        return (uint32_t)frame;

    if (current != 0 &&
        current->state == TASK_RUNNING) {

        current->state = TASK_READY;
    }

    selected->state = TASK_RUNNING;
    current_task_id = next;

    scheduler_activate(selected);
    scheduler_display(next);

    return selected->switch_esp;
}


/*
 * Immediately switch away from a task that has executed SYS_EXIT.
 *
 * The exiting task must never return through its own syscall iret
 * frame. Its address space is kept alive until its parent reaps it
 * with task_wait().
 */
uint32_t scheduler_exit_current(uint32_t *frame)
{
    uint32_t current_id = current_task_id;
    task_t *current = (task_t *)task_get(current_id);

    /*
     * Keep the frame associated with the task for bookkeeping, but
     * this frame must NOT be returned to iret for a terminated task.
     */
    if (current != 0 && frame != 0)
        current->switch_esp = (uint32_t)frame;

    /*
     * Find the next runnable task.
     */
    uint32_t next_id = scheduler_next_task();

    /*
     * Never return the terminated task itself.
     */
    if (next_id == current_id || next_id > MAX_TASKS)
        next_id = 0;

    task_t *next = (task_t *)task_get(next_id);

    /*
     * Task 0 is the ultimate fallback. If it has no saved frame,
     * preserve the old frame rather than dereferencing NULL.
     */
    if (next == 0 ||
        next->state == TASK_UNUSED ||
        next->state == TASK_TERMINATED ||
        next->switch_esp == 0) {
        return (uint32_t)frame;
    }

    next->state = TASK_RUNNING;
    current_task_id = next_id;

    scheduler_activate(next);

    return next->switch_esp;
}

int scheduler_run_self_test(void)
{
    uint32_t saved_current = current_task_id;

    int first = task_create();
    if (first < 0)
        return 0;

    int second = task_create();
    if (second < 0) {
        task_terminate((uint32_t)first);
        return 0;
    }

    current_task_id = 0;

    if (scheduler_next_task() != (uint32_t)first) {
        task_terminate((uint32_t)first);
        task_terminate((uint32_t)second);
        current_task_id = saved_current;
        return 0;
    }

    if (task_block((uint32_t)first) != 0) {
        task_terminate((uint32_t)first);
        task_terminate((uint32_t)second);
        current_task_id = saved_current;
        return 0;
    }

    if (scheduler_next_task() != (uint32_t)second) {
        task_terminate((uint32_t)first);
        task_terminate((uint32_t)second);
        current_task_id = saved_current;
        return 0;
    }

    if (task_wake((uint32_t)first) != 0) {
        task_terminate((uint32_t)first);
        task_terminate((uint32_t)second);
        current_task_id = saved_current;
        return 0;
    }

    if (scheduler_next_task() != (uint32_t)first) {
        task_terminate((uint32_t)first);
        task_terminate((uint32_t)second);
        current_task_id = saved_current;
        return 0;
    }

    task_terminate((uint32_t)first);
    task_terminate((uint32_t)second);

    current_task_id = saved_current;

    return 1;
}
