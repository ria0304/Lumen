#include <stdint.h>
#include "task.h"
#include "console.h"

extern void task_demo_entry(void);

static task_t tasks[MAX_TASKS + 1];

/*
 * Fixed kernel stacks live in BSS instead of consuming the
 * small bump heap. This gives every kernel task a permanent
 * 4 KiB stack and allows terminated task slots to be reused.
 */
static uint8_t task_stacks[MAX_TASKS + 1][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

static uint32_t active_tasks = 0;

static uint32_t task_prepare_stack(
    uint32_t stack_base,
    uint32_t stack_size,
    uint32_t entry
)
{
    uint32_t *sp =
        (uint32_t *)(stack_base + stack_size);

    /*
     * Final interrupt-return layout:
     *
     * EDI
     * ESI
     * EBP
     * ESP dummy
     * EBX
     * EDX
     * ECX
     * EAX
     * EIP
     * CS
     * EFLAGS
     */
    *--sp = 0x00000202;
    *--sp = KERNEL_CODE_SELECTOR;
    *--sp = entry;

    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

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

    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].privilege = KERNEL_RING;
    tasks[0].stack_base = 0x90000;
    tasks[0].stack_size = 0x10000;

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

        tasks[i].id = i;
        tasks[i].state = TASK_READY;
        tasks[i].privilege = privilege;

        tasks[i].stack_base =
            (uint32_t)&task_stacks[i][0];

        tasks[i].stack_size = TASK_STACK_SIZE;

        tasks[i].context.eax = 0;
        tasks[i].context.ebx = 0;
        tasks[i].context.ecx = 0;
        tasks[i].context.edx = 0;
        tasks[i].context.esi = 0;
        tasks[i].context.edi = 0;

        tasks[i].context.esp =
            tasks[i].stack_base + TASK_STACK_SIZE;

        tasks[i].context.ebp =
            tasks[i].stack_base + TASK_STACK_SIZE;

        tasks[i].context.eip =
            (uint32_t)task_demo_entry;

        tasks[i].context.eflags = 0x202;

        /*
         * Kernel-ring tasks participate in the current
         * preemptive context-switch path.
         *
         * Ring-3 task execution remains a separate user-mode
         * task milestone and is deliberately not mixed into
         * this scheduler-core completion.
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

        return (int)i;
    }

    return -1;
}

int task_create(void)
{
    return task_create_with_privilege(KERNEL_RING);
}

int task_terminate(uint32_t id)
{
    if (id == 0 || id > MAX_TASKS)
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

int task_block(uint32_t id)
{
    if (id == 0 || id > MAX_TASKS)
        return -1;

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_READY &&
        task->state != TASK_RUNNING)
        return -1;

    task->state = TASK_BLOCKED;
    return 0;
}

int task_wake(uint32_t id)
{
    if (id > MAX_TASKS)
        return -1;

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_BLOCKED)
        return -1;

    task->state = TASK_READY;
    return 0;
}

extern uint32_t scheduler_current_task(void);

int task_yield(void)
{
    uint32_t id = scheduler_current_task();

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_RUNNING)
        return -1;

    /*
     * The next timer interrupt performs the actual switch.
     */
    task->state = TASK_READY;

    return 0;
}

int task_exit(void)
{
    uint32_t id = scheduler_current_task();

    if (id == 0)
        return -1;

    return task_terminate(id);
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

int task_run_self_test(void)
{
    uint32_t before = active_tasks;

    int first = task_create();
    if (first < 0)
        return 0;

    int second = task_create();
    if (second < 0) {
        task_terminate((uint32_t)first);
        return 0;
    }

    const task_t *a = task_get((uint32_t)first);
    const task_t *b = task_get((uint32_t)second);

    if (a == 0 || b == 0)
        return 0;

    if (a->stack_base == b->stack_base)
        return 0;

    if (a->stack_size != TASK_STACK_SIZE ||
        b->stack_size != TASK_STACK_SIZE)
        return 0;

    if (a->state != TASK_READY ||
        b->state != TASK_READY)
        return 0;

    if (a->switch_esp == 0 ||
        b->switch_esp == 0)
        return 0;

    if (task_block((uint32_t)first) != 0)
        return 0;

    if (a->state != TASK_BLOCKED)
        return 0;

    if (task_wake((uint32_t)first) != 0)
        return 0;

    if (a->state != TASK_READY)
        return 0;

    if (task_terminate((uint32_t)first) != 0)
        return 0;

    if (task_terminate((uint32_t)second) != 0)
        return 0;

    if (active_tasks != before)
        return 0;

    return 1;
}
