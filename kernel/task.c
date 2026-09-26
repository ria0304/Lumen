#include <stdint.h>
#include "task.h"
#include "console.h"
#include "heap.h"

extern void task_demo_entry(void);

static task_t tasks[MAX_TASKS + 1];
static uint32_t active_tasks = 0;

/*
 * Build the stack that POPA + IRET expects.
 *
 * Final layout at switch_esp:
 *
 *   EDI
 *   ESI
 *   EBP
 *   ESP dummy
 *   EBX
 *   EDX
 *   ECX
 *   EAX
 *   EIP
 *   CS
 *   EFLAGS
 */
static uint32_t task_prepare_stack(
    uint32_t stack_base,
    uint32_t stack_size,
    uint32_t entry
)
{
    uint32_t *sp =
        (uint32_t *)(stack_base + stack_size);

    *--sp = 0x00000202;                 /* EFLAGS */
    *--sp = KERNEL_CODE_SELECTOR;       /* CS */
    *--sp = entry;                       /* EIP */

    *--sp = 0;                           /* EAX */
    *--sp = 0;                           /* ECX */
    *--sp = 0;                           /* EDX */
    *--sp = 0;                           /* EBX */
    *--sp = 0;                           /* ESP - ignored by POPA */
    *--sp = 0;                           /* EBP */
    *--sp = 0;                           /* ESI */
    *--sp = 0;                           /* EDI */

    return (uint32_t)sp;
}

void task_init(void)
{
    for (uint32_t i = 0; i <= MAX_TASKS; i++) {
        tasks[i].id = 0;
        tasks[i].state = TASK_UNUSED;
        tasks[i].privilege = KERNEL_RING;

        tasks[i].stack_base = 0;
        tasks[i].stack_size = 0;
        tasks[i].switch_esp = 0;

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

    /*
     * Task 0 represents the kernel/shell execution context.
     * Its interrupt frame is captured the first time the
     * scheduler switches away from it.
     */
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].privilege = KERNEL_RING;

    active_tasks = 0;

    console_info("Task manager initialized: OK");
}

int task_create_with_privilege(uint32_t privilege)
{
    if (privilege != KERNEL_RING &&
        privilege != USER_RING)
        return -1;

    for (uint32_t i = 1; i <= MAX_TASKS; i++) {

        if (tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_TERMINATED)
            continue;

        void *stack = kmalloc(TASK_STACK_SIZE);

        if (stack == 0)
            return -1;

        tasks[i].id = i;
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

        tasks[i].context.esp =
            (uint32_t)stack + TASK_STACK_SIZE;

        tasks[i].context.ebp =
            (uint32_t)stack + TASK_STACK_SIZE;

        tasks[i].context.eip =
            (uint32_t)task_demo_entry;

        tasks[i].context.eflags = 0x202;

        /*
         * Day 24 performs actual switching only for
         * kernel-ring tasks.
         *
         * Ring 3 tasks remain available as scheduler
         * metadata until the user-mode stack/TSS path
         * is completed.
         */
        if (privilege == KERNEL_RING) {
            tasks[i].switch_esp =
                task_prepare_stack(
                    tasks[i].stack_base,
                    tasks[i].stack_size,
                    (uint32_t)task_demo_entry
                );
        } else {
            tasks[i].switch_esp = 0;
        }

        active_tasks++;

        return (int)tasks[i].id;
    }

    return -1;
}

int task_create(void)
{
    return task_create_with_privilege(KERNEL_RING);
}

int task_terminate(uint32_t id)
{
    if (id == 0)
        return -1;

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state == TASK_UNUSED ||
        task->state == TASK_TERMINATED)
        return -1;

    task->state = TASK_TERMINATED;

    if (active_tasks > 0)
        active_tasks--;

    return 0;
}

const task_t *task_get(uint32_t id)
{
    if (id > MAX_TASKS)
        return 0;

    if (tasks[id].state == TASK_UNUSED)
        return 0;

    return &tasks[id];
}

uint32_t task_count(void)
{
    return active_tasks;
}
