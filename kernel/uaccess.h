#ifndef UACCESS_H
#define UACCESS_H

#include <stdint.h>

#include "ring3.h"

/*
 * User address window: Ring 3 tasks may only pass pointers in
 * [USER_ADDR_MIN, USER_ADDR_MAX). Everything at or above
 * USER_ADDR_MAX is kernel memory and must never be touched
 * through a user-supplied pointer.
 */
#define USER_ADDR_MIN TASK_RING3_CODE_VA
#define USER_ADDR_MAX 0xC0000000U

/* Max NUL-terminated string (path, etc.) accepted from Ring 3. */
#define UACCESS_MAX_STRING 256U

/*
 * Validate that [uaddr, uaddr+len) lies inside the user window,
 * does not wrap around, and every page is present with the
 * USER bit (plus WRITE when writable is nonzero) in the given
 * page directory (physical address).
 *
 * Returns 0 if the whole range is safe, -1 otherwise.
 * A zero length is always accepted.
 */
int uaccess_check(
    uint32_t directory_physical,
    uint32_t uaddr,
    uint32_t len,
    int writable
);

/*
 * Copy between kernel memory and a validated user range.
 * Returns 0 on success, -1 on a bad pointer.
 */
int copy_from_user(
    void *dst,
    uint32_t usrc,
    uint32_t len,
    uint32_t directory_physical
);

int copy_to_user(
    uint32_t udst,
    const void *src,
    uint32_t len,
    uint32_t directory_physical
);

/*
 * Copy a NUL-terminated string from user space, bounding the
 * scan at maxlen bytes (including the NUL). Fails when no NUL
 * appears in time or any touched page is not user-readable.
 * Returns the string length (without NUL) or -1.
 */
int copy_string_from_user(
    char *dst,
    uint32_t usrc,
    uint32_t maxlen,
    uint32_t directory_physical
);

/*
 * Read one 32-bit word from a validated user range. Returns 0 on
 * success and -1 when the address is not readable, so a caller can
 * never mistake a rejected read for a value.
 */
int uaccess_read_u32(
    uint32_t directory_physical,
    uint32_t uaddr,
    uint32_t *out
);

/* Boot self-test: unit checks plus a live Ring 3 probe task. */
int syscall_run_self_test(void);

#endif
