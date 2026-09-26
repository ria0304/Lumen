#include <stdint.h>
#include "task.h"
#include "console.h"
#include "frame.h"
#include "paging.h"
#include "ring3.h"

extern void task_demo_entry(void);

static task_t tasks[MAX_TASKS + 1];

/*
 * Fixed kernel stacks live in BSS. Every task, kernel-ring or
 * user-ring, gets a permanently dedicated 4 KiB stack here. For
 * Ring 3 tasks this doubles as the TSS.esp0 target while that
 * task is current -- each task's interrupt/syscall entry frame
 * lands on ITS OWN stack, never aliasing task 0's or any other
 * task's.
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

/*
 * Same idea, but for a Ring 3 task's FIRST entry. IRET detects
 * that the CS being restored (USER_CODE_SELECTOR, RPL=3) differs
 * from the current CPL and automatically pops the extra ESP/SS
 * pair too, performing the Ring 0 -> Ring 3 transition. No
 * special-cased assembly is needed for this -- the existing
 * irq0 popa/iret path in isr.asm handles both frame shapes.
 *
 * Final layout:
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
 * ESP (user)
 * SS  (user)
 */
static uint32_t task_prepare_user_stack(
    uint32_t stack_base,
    uint32_t stack_size,
    uint32_t entry,
    uint32_t user_esp
)
{
    uint32_t *sp =
        (uint32_t *)(stack_base + stack_size);

    *--sp = USER_DATA_SELECTOR;
    *--sp = user_esp;
    *--sp = 0x00000202;
    *--sp = USER_CODE_SELECTOR;
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
        tasks[i].page_directory = PAGE_DIRECTORY_ADDRESS;

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
     * It never faults from Ring 3, so its own stack_base/size
     * (and whatever TSS.esp0 happens to hold while it runs) are
     * never actually used by hardware -- they only matter for
     * tasks that DO take a privilege-changing interrupt.
     */
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].privilege = KERNEL_RING;
    tasks[0].page_directory = PAGE_DIRECTORY_ADDRESS;
    tasks[0].stack_base = 0x90000;
    tasks[0].stack_size = 0x10000;

    active_tasks = 0;

    console_info("Task manager initialized: OK");
}

/*
 * Shared Ring 3 task setup: builds an isolated address space,
 * copies 'code' (up to PAGE_SIZE bytes) into a dedicated code
 * frame, gives it a dedicated zeroed stack frame, and prepares
 * the interrupt-return frame that lands it in Ring 3 at
 * TASK_RING3_CODE_VA. Used by both the fixed-demo path
 * (task_create_with_privilege(USER_RING)) and the general
 * loader path (task_create_user_program()) so there is exactly
 * one place that gets this sequence right.
 */
static int task_create_ring3_with_code(
    const uint8_t *code,
    uint32_t code_size
)
{
    if (code_size > PAGE_SIZE)
        return -1;

    uint32_t i;

    for (i = 1; i <= MAX_TASKS; i++) {

        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_TERMINATED)
            break;
    }

    if (i > MAX_TASKS)
        return -1;

    tasks[i].id = i;
    tasks[i].state = TASK_READY;
    tasks[i].privilege = USER_RING;

    tasks[i].stack_base = (uint32_t)&task_stacks[i][0];
    tasks[i].stack_size = TASK_STACK_SIZE;

    tasks[i].context.eax = 0;
    tasks[i].context.ebx = 0;
    tasks[i].context.ecx = 0;
    tasks[i].context.edx = 0;
    tasks[i].context.esi = 0;
    tasks[i].context.edi = 0;
    tasks[i].context.eflags = 0x202;

    __asm__ volatile ("cli");

    uint32_t directory = paging_create_address_space();

    if (directory == FRAME_INVALID) {
        console_error("User task: directory FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint32_t code_phys = frame_alloc();

    if (code_phys == FRAME_INVALID ||
        code_phys >= PAGING_IDENTITY_LIMIT) {

        if (code_phys != FRAME_INVALID)
            frame_free(code_phys);

        frame_free(directory);
        console_error("User task: code frame FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint8_t *code_dst = (uint8_t *)code_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        code_dst[k] = (k < code_size) ? code[k] : 0;

    if (paging_map_in_directory(
            directory,
            TASK_RING3_CODE_VA,
            code_phys,
            PAGE_PRESENT | PAGE_USER
        ) != 0) {

        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: code map FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint32_t stack_phys = frame_alloc();

    if (stack_phys == FRAME_INVALID ||
        stack_phys >= PAGING_IDENTITY_LIMIT) {

        if (stack_phys != FRAME_INVALID)
            frame_free(stack_phys);

        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: stack frame FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint8_t *stack_dst = (uint8_t *)stack_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        stack_dst[k] = 0;

    if (paging_map_in_directory(
            directory,
            TASK_RING3_STACK_VA,
            stack_phys,
            PAGE_PRESENT | PAGE_WRITE | PAGE_USER
        ) != 0) {

        frame_free(stack_phys);
        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: stack map FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    tasks[i].page_directory = directory;

    tasks[i].switch_esp =
        task_prepare_user_stack(
            tasks[i].stack_base,
            tasks[i].stack_size,
            TASK_RING3_CODE_VA,
            TASK_RING3_STACK_TOP
        );

    __asm__ volatile ("sti");

    active_tasks++;

    return (int)i;
}

int task_create_user_program(const uint8_t *code, uint32_t code_size)
{
    return task_create_ring3_with_code(code, code_size);
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
        tasks[i].page_directory = PAGE_DIRECTORY_ADDRESS;

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

        if (privilege == KERNEL_RING) {

            tasks[i].switch_esp =
                task_prepare_stack(
                    tasks[i].stack_base,
                    tasks[i].stack_size,
                    (uint32_t)task_demo_entry
                );

        } else {

            __asm__ volatile ("cli");

            /*
             * Build a dedicated, isolated address space: its
             * own page directory, its own physical code/stack
             * frames, mapped only inside that directory. If
             * anything here fails, unwind everything already
             * allocated and refuse to create the task rather
             * than leave it half-built.
             */
            uint32_t directory =
                paging_create_address_space();

            if (directory == FRAME_INVALID) {
                console_error("Ring 3 task: directory FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint32_t code_phys =
                frame_alloc();

            if (code_phys == FRAME_INVALID ||
                code_phys >= PAGING_IDENTITY_LIMIT) {

                if (code_phys != FRAME_INVALID)
                    frame_free(code_phys);

                frame_free(directory);
                console_error("Ring 3 task: code frame FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint8_t *code_dst =
                (uint8_t *)code_phys;

            for (uint32_t k = 0;
                 k < ring3_test_program_size;
                 k++) {

                code_dst[k] =
                    ring3_test_program[k];
            }

            if (paging_map_in_directory(
                    directory,
                    TASK_RING3_CODE_VA,
                    code_phys,
                    PAGE_PRESENT | PAGE_USER
                ) != 0) {

                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: code map FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint32_t stack_phys =
                frame_alloc();

            if (stack_phys == FRAME_INVALID ||
                stack_phys >= PAGING_IDENTITY_LIMIT) {

                if (stack_phys != FRAME_INVALID)
                    frame_free(stack_phys);

                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: stack frame FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint8_t *stack_dst =
                (uint8_t *)stack_phys;

            for (uint32_t k = 0;
                 k < PAGE_SIZE;
                 k++) {

                stack_dst[k] = 0;
            }

            if (paging_map_in_directory(
                    directory,
                    TASK_RING3_STACK_VA,
                    stack_phys,
                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER
                ) != 0) {

                frame_free(stack_phys);
                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: stack map FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            tasks[i].page_directory = directory;

            tasks[i].switch_esp =
                task_prepare_user_stack(
                    tasks[i].stack_base,
                    tasks[i].stack_size,
                    TASK_RING3_CODE_VA,
                    TASK_RING3_STACK_TOP
                );

            __asm__ volatile ("sti");
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

    if (task->privilege == USER_RING &&
        task->page_directory != PAGE_DIRECTORY_ADDRESS) {

        __asm__ volatile ("cli");

        /*
         * If this task's directory is the one currently loaded
         * in CR3 -- true when a task is being terminated from
         * inside its own fault handler, before the scheduler has
         * had a chance to switch away from it -- get CR3 off of
         * it FIRST. paging_destroy_address_space() frees the
         * directory's physical frame; tearing down the directory
         * that is still actively translating every memory access
         * this very code is making is what used to crash the
         * kernel immediately after printing the termination
         * message.
         */
        if (paging_current_directory() == task->page_directory) {
            paging_switch_directory(PAGE_DIRECTORY_ADDRESS);
        }

        paging_destroy_address_space(
            task->page_directory
        );

        task->page_directory = PAGE_DIRECTORY_ADDRESS;

        __asm__ volatile ("sti");
    }

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
