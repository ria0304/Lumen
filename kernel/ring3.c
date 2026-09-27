#include <stdint.h>

#include "ring3.h"
#include "paging.h"
#include "console.h"

/*
 * Tiny Ring 3 program:
 *
 *     mov eax, 1
 *     int 0x80
 *     jmp $
 *
 * EAX=1 means "Ring 3 test syscall". Shared between the legacy
 * one-shot 'usermode' path and per-task 'taskuser' spawns so
 * there's exactly one copy of the bytes.
 */
const uint8_t ring3_test_program[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xEB, 0xFE
};

const uint32_t ring3_test_program_size =
    sizeof(ring3_test_program);

/*
 * Ring 3 process-lifecycle test:
 *
 *     mov eax, 3
 *     int 0x80
 *     jmp $
 *
 * SYS_EXIT=3. The task should terminate and disappear from the
 * runnable scheduler set.
 */
const uint8_t ring3_exit_program[] = {
    0xB8, 0x03, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xEB, 0xFE
};

const uint32_t ring3_exit_program_size =
    sizeof(ring3_exit_program);

extern void enter_user_mode(void);

static int ring3_ready = 0;
static uint32_t code_physical = 0;
static uint32_t stack_physical = 0;

int ring3_init(void)
{
    if (ring3_ready)
        return 1;

    code_physical =
        paging_alloc_page(
            RING3_CODE_VA,
            PAGE_USER
        );

    if (code_physical == 0xFFFFFFFFU) {
        console_error("Ring 3 code page: FAILED");
        return 0;
    }

    stack_physical =
        paging_alloc_page(
            RING3_STACK_VA,
            PAGE_WRITE | PAGE_USER
        );

    if (stack_physical == 0xFFFFFFFFU) {
        console_error("Ring 3 stack page: FAILED");
        paging_free_page(RING3_CODE_VA);
        code_physical = 0;
        return 0;
    }

    volatile uint8_t *stack =
        (volatile uint8_t *)stack_physical;

    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        stack[i] = 0;

    volatile uint8_t *code =
        (volatile uint8_t *)code_physical;

    for (uint32_t i = 0; i < ring3_test_program_size; i++)
        code[i] = ring3_test_program[i];

    ring3_ready = 1;

    console_info("Ring 3 code page: OK");
    console_info("Ring 3 stack page: OK");
    console_info("Ring 3 memory setup: PASS");

    return 1;
}

int ring3_run_test(void)
{
    if (!ring3_ready) {
        if (!ring3_init())
            return 0;
    }

    console_info("Entering Ring 3...");

    enter_user_mode();

    console_error("Ring 3 returned unexpectedly");

    return 0;
}
