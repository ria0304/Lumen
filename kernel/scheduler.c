#include <stdint.h>
#include "scheduler.h"
#include "task.h"
#include "console.h"

static uint32_t current_task_id = 0;

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
    for (uint32_t id = current_task_id + 1;
         id <= MAX_TASKS;
         id++) {

        const task_t *task = task_get(id);

        if (task != 0 &&
            task->state == TASK_READY) {
            return id;
        }
    }

    for (uint32_t id = 1;
         id <= current_task_id;
         id++) {

        const task_t *task = task_get(id);

        if (task != 0 &&
            task->state == TASK_READY) {
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

    if (current != 0 &&
        current->state == TASK_RUNNING) {

        current->state = TASK_READY;
    }

    task_t *selected =
        (task_t *)task_get(next);

    if (selected != 0 &&
        selected->state == TASK_READY) {

        selected->state = TASK_RUNNING;
        current_task_id = next;
    }
}
