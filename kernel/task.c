#include <stdint.h>
#include "task.h"
#include "console.h"
#include "heap.h"

static task_t tasks[MAX_TASKS];
static uint32_t next_task_id = 1;
static uint32_t active_tasks = 0;

void task_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].id = 0;
        tasks[i].state = TASK_UNUSED;
        tasks[i].privilege = KERNEL_RING;
        tasks[i].stack_base = 0;
        tasks[i].stack_size = 0;

        tasks[i].context.eax = 0;
        tasks[i].context.ebx = 0;
        tasks[i].context.ecx = 0;
        tasks[i].context.edx = 0;
        tasks[i].context.esi = 0;
        tasks[i].context.edi = 0;
        tasks[i].context.ebp = 0;
        tasks[i].context.esp = 0;
        tasks[i].context.eip = 0;
        tasks[i].context.eflags = 0x202;
    }

    next_task_id = 1;
    active_tasks = 0;

    console_info("Task manager initialized: OK");
}

int task_create_with_privilege(uint32_t privilege)
{
    if (privilege != KERNEL_RING && privilege != USER_RING)
        return -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {

            void *stack = kmalloc(TASK_STACK_SIZE);

            if (stack == 0)
                return -1;

            tasks[i].id = next_task_id++;
            tasks[i].state = TASK_READY;
            tasks[i].privilege = privilege;

            tasks[i].stack_base = (uint32_t)stack;
            tasks[i].stack_size = TASK_STACK_SIZE;

            tasks[i].context.eax = 0;
            tasks[i].context.ebx = 0;
            tasks[i].context.ecx = 0;
            tasks[i].context.edx = 0;
            tasks[i].context.esi = 0;
            tasks[i].context.edi = 0;
            tasks[i].context.ebp =
                (uint32_t)stack + TASK_STACK_SIZE;

            tasks[i].context.esp =
                (uint32_t)stack + TASK_STACK_SIZE;

            tasks[i].context.eip = 0;
            tasks[i].context.eflags = 0x202;

            active_tasks++;

            return (int)tasks[i].id;
        }
    }

    return -1;
}

int task_create(void)
{
    return task_create_with_privilege(KERNEL_RING);
}

int task_terminate(uint32_t id)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].id == id &&
            tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_TERMINATED) {

            tasks[i].state = TASK_TERMINATED;

            if (active_tasks > 0)
                active_tasks--;

            return 0;
        }
    }

    return -1;
}

const task_t *task_get(uint32_t id)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].id == id)
            return &tasks[i];
    }

    return 0;
}

uint32_t task_count(void)
{
    return active_tasks;
}
