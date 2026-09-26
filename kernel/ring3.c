#include <stdint.h>

#include "ring3.h"
#include "paging.h"
#include "console.h"

/*
 * Tiny Ring-3 program:
 *
 *     mov eax, 1
 *     int 0x80
 *     jmp $
 *
 * EAX=1 means "Ring-3 test syscall".
 */
static const uint8_t user_code[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xEB, 0xFE
};

extern void enter_user_mode(void);

static int ring3_ready = 0;
static uint32_t code_physical = 0;
static uint32_t stack_physical = 0;

int ring3_init(void)
{
    if (ring3_ready)
        return 1;

    /*
     * Allocate user code page.
     *
     * The existing paging allocator returns the physical
     * frame. The initial frame allocator operates below
     * the first 4 MiB, so the kernel can access the physical
     * frame through the existing identity mapping.
     */
    code_physical =
        paging_alloc_page(
            RING3_CODE_VA,
            PAGE_USER
        );

    if (code_physical == 0) {
        console_error("Ring 3 code page: FAILED");
        return 0;
    }

    /*
     * Allocate writable user stack.
     */
    stack_physical =
        paging_alloc_page(
            RING3_STACK_VA,
            PAGE_WRITE | PAGE_USER
        );

    if (stack_physical == 0) {
        console_error("Ring 3 stack page: FAILED");
        paging_free_page(RING3_CODE_VA);
        code_physical = 0;
        return 0;
    }

    /*
     * Clear the user stack through the physical identity map.
     */
    volatile uint8_t *stack =
        (volatile uint8_t *)stack_physical;

    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        stack[i] = 0;

    /*
     * Copy user program into its physical frame.
     */
    volatile uint8_t *code =
        (volatile uint8_t *)code_physical;

    for (uint32_t i = 0; i < sizeof(user_code); i++)
        code[i] = user_code[i];

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

    /*
     * We should never reach this point because the Ring-3
     * program loops forever after returning from syscall.
     */
    console_error("Ring 3 returned unexpectedly");

    return 0;
}
