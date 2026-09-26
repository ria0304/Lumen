#include <stdint.h>
#include "scheduler.h"
#include "task.h"
#include "console.h"

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

    /*
     * frame[3] is the ESP value captured by PUSHA.
     * The actual ESP before PUSHA is frame + 32.
     */
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

    uint32_t value = id;

    if (value < 10) {
        vga[base + 8] =
            ((uint16_t)0x07 << 8) |
            (uint8_t)('0' + value);
    }
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
     * Search all task IDs after the current task.
     */
    for (uint32_t id = current_task_id + 1;
         id <= MAX_TASKS;
         id++) {

        const task_t *task = task_get(id);

        if (task != 0 &&
            task->state == TASK_READY &&
            task->privilege == KERNEL_RING &&
            task->switch_esp != 0) {

            return id;
        }
    }

    /*
     * Wrap around.
     */
    for (uint32_t id = 0;
         id <= current_task_id &&
         id <= MAX_TASKS;
         id++) {

        const task_t *task = task_get(id);

        if (task != 0 &&
            task->state == TASK_READY &&
            task->privilege == KERNEL_RING &&
            task->switch_esp != 0) {

            return id;
        }
    }

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

        scheduler_display(next);
    }
}

uint32_t scheduler_irq(uint32_t *frame)
{
    /*
     * timer_handler updates the timer display and
     * acknowledges the PIC before the context switch.
     */
    timer_handler();

    /*
     * Save the context of the task that was interrupted.
     */
    task_t *current =
        (task_t *)task_get(current_task_id);

    if (current != 0)
        save_task_context(current, frame);

    /*
     * Select the next runnable task.
     */
    uint32_t next = scheduler_next_task();

    if (next == current_task_id)
        return (uint32_t)frame;

    task_t *selected =
        (task_t *)task_get(next);

    if (selected == 0 ||
        selected->switch_esp == 0)
        return (uint32_t)frame;

    /*
     * Current task becomes READY unless it has
     * already been terminated.
     */
    if (current != 0 &&
        current->state == TASK_RUNNING) {

        current->state = TASK_READY;
    }

    selected->state = TASK_RUNNING;
    current_task_id = next;

    scheduler_display(next);

    /*
     * IRQ0 will:
     *
     *   mov esp, returned_value
     *   popa
     *   iret
     *
     * This is the actual context switch.
     */
    return selected->switch_esp;
}
