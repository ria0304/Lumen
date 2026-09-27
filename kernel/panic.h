#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kernel panic and assertion facility.
 *
 * Panic halts the system and prints diagnostic information.
 * Assertion checks a condition and panics if it fails.
 */

#define KERNEL_PANIC(fmt, ...) \
    do { \
        terminal_write("[KERNEL PANIC] "); \
        terminal_write(fmt); \
        terminal_putchar('\n'); \
        for (;;) { \
            __asm__ volatile ("cli; hlt"); \
        } \
    } while (0)

#define KERNEL_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            KERNEL_PANIC("Assertion failed: %s", #cond); \
        } \
    } while (0)

#define KERNEL_ASSERT_NOT_NULL(ptr) \
    KERNEL_ASSERT((ptr) != 0, "null pointer")

#define KERNEL_ASSERT_VALID_IDX(idx, max) \
    KERNEL_ASSERT((idx) < (max), "index out of bounds")

#ifdef __cplusplus
}
#endif

#endif /* PANIC_H */