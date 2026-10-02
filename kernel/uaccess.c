#include <stdint.h>

#include "uaccess.h"
#include "paging.h"
#include "task.h"
#include "scheduler.h"
#include "console.h"


extern volatile uint32_t timer_ticks;

int uaccess_check(
    uint32_t directory_physical,
    uint32_t uaddr,
    uint32_t len,
    int writable
)
{
    if (len == 0)
        return 0;

    /* Below the user window or at/above the kernel boundary. */
    if (uaddr < USER_ADDR_MIN || uaddr >= USER_ADDR_MAX)
        return -1;

    /* Length overflow / wraparound, or past the boundary. */
    if (len > USER_ADDR_MAX - uaddr)
        return -1;

    uint32_t start_page = uaddr & ~(PAGE_SIZE - 1U);
    uint32_t end = uaddr + len;
    uint32_t end_page = (end - 1U) & ~(PAGE_SIZE - 1U);

    for (uint32_t page = start_page; ; page += PAGE_SIZE) {
        uint32_t flags =
            paging_flags_from_directory(
                directory_physical,
                page
            );

        if ((flags & PAGE_PRESENT) == 0)
            return -1;

        if ((flags & PAGE_USER) == 0)
            return -1;

        if (writable && (flags & PAGE_WRITE) == 0)
            return -1;

        if (page == end_page)
            break;
    }

    return 0;
}

/*
 * Translate one user byte to a kernel-dereferenceable address.
 * User frames live below PAGING_IDENTITY_LIMIT, so their
 * physical address doubles as a kernel virtual address.
 */
static uint8_t *uaccess_byte(
    uint32_t directory_physical,
    uint32_t uaddr,
    int writable
)
{
    uint32_t flags =
        paging_flags_from_directory(
            directory_physical,
            uaddr
        );

    if ((flags & PAGE_PRESENT) == 0)
        return 0;

    if ((flags & PAGE_USER) == 0)
        return 0;

    if (writable && (flags & PAGE_WRITE) == 0)
        return 0;

    uint32_t phys =
        paging_get_physical_from_directory(
            directory_physical,
            uaddr
        );

    uint32_t page_phys = phys & ~(PAGE_SIZE - 1U);

    if (page_phys >= PAGING_IDENTITY_LIMIT)
        return 0;

    return (uint8_t *)phys;
}

int copy_from_user(
    void *dst,
    uint32_t usrc,
    uint32_t len,
    uint32_t directory_physical
)
{
    if (len == 0)
        return 0;

    if (dst == 0)
        return -1;

    if (uaccess_check(directory_physical, usrc, len, 0) != 0)
        return -1;

    uint8_t *d = (uint8_t *)dst;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t *s =
            uaccess_byte(directory_physical, usrc + i, 0);

        if (s == 0)
            return -1;

        d[i] = *s;
    }

    return 0;
}

int copy_to_user(
    uint32_t udst,
    const void *src,
    uint32_t len,
    uint32_t directory_physical
)
{
    if (len == 0)
        return 0;

    if (src == 0)
        return -1;

    if (uaccess_check(directory_physical, udst, len, 1) != 0)
        return -1;

    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t *d =
            uaccess_byte(directory_physical, udst + i, 1);

        if (d == 0)
            return -1;

        *d = s[i];
    }

    return 0;
}

int copy_string_from_user(
    char *dst,
    uint32_t usrc,
    uint32_t maxlen,
    uint32_t directory_physical
)
{
    if (dst == 0 || maxlen == 0)
        return -1;

    if (usrc < USER_ADDR_MIN || usrc >= USER_ADDR_MAX)
        return -1;

    for (uint32_t i = 0; i < maxlen; i++) {
        /* Re-check each page; the pre-check alone could miss
         * a string that runs past one validated page. */
        if (usrc + i < usrc)
            return -1;

        if (usrc + i >= USER_ADDR_MAX)
            return -1;

        uint8_t *s =
            uaccess_byte(directory_physical, usrc + i, 0);

        if (s == 0)
            return -1;

        dst[i] = (char)*s;

        if (*s == 0)
            return (int)i;
    }

    return -1;
}

int uaccess_read_u32(
    uint32_t directory_physical,
    uint32_t uaddr,
    uint32_t *out
)
{
    if (out == 0)
        return -1;

    /* Validate first: a rejected read must be distinguishable from a
     * value of 0, which uaccess_read_word()'s caller cannot do. */
    if (uaccess_check(directory_physical, uaddr, 4, 0) != 0)
        return -1;

    uint8_t *src = uaccess_byte(directory_physical, uaddr, 0);

    if (src == 0)
        return -1;

    /* Assembled byte by byte: the mapping is only guaranteed valid for
     * the single byte we asked about, and the CPU cannot be relied on
     * to fetch all four within one page. */
    uint32_t value = 0;

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *b = uaccess_byte(directory_physical, uaddr + i, 0);

        if (b == 0)
            return -1;

        value |= ((uint32_t)*b) << (i * 8);
    }

    *out = value;

    return 0;
}

/*
 * Live probe, executed in Ring 3 by a real user task:
 *
 *     mov eax, 21                  ; SYS_GETTIME
 *     mov ebx, 0x00100000          ; kernel address -> must fail
 *     int 0x80
 *     mov [0x01001FFC], eax        ; expect 0xFFFFFFFF
 *     mov eax, 21
 *     mov ebx, 0x01001F00          ; own writable stack -> must work
 *     int 0x80
 *     mov [0x01001FF8], eax        ; expect 0
 *     mov eax, 3                   ; SYS_EXIT
 *     int 0x80
 *     jmp $
 */
static const uint8_t uaccess_probe[] = {
    0xB8, 0x15, 0x00, 0x00, 0x00,
    0xBB, 0x00, 0x00, 0x10, 0x00,
    0xCD, 0x80,
    0xA3, 0xFC, 0x1F, 0x00, 0x01,
    0xB8, 0x15, 0x00, 0x00, 0x00,
    0xBB, 0x00, 0x1F, 0x00, 0x01,
    0xCD, 0x80,
    0xA3, 0xF8, 0x1F, 0x00, 0x01,
    0xB8, 0x03, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xEB, 0xFE
};

#define UACCESS_RES_BAD 0x01001FFCU
#define UACCESS_RES_GOOD 0x01001FF8U
#define UACCESS_GOOD_BUF 0x01001F00U

static uint32_t uaccess_read_word(
    uint32_t directory_physical,
    uint32_t uaddr
)
{
    uint32_t phys =
        paging_get_physical_from_directory(
            directory_physical,
            uaddr
        );

    if ((phys & ~(PAGE_SIZE - 1U)) >= PAGING_IDENTITY_LIMIT)
        return 0xDEADBEEFU;

    return *(volatile uint32_t *)phys;
}

int syscall_run_self_test(void)
{
    int id =
        task_create_user_program(
            uaccess_probe,
            sizeof(uaccess_probe)
        );

    if (id < 0)
        return 0;

    const task_t *probe = task_get((uint32_t)id);

    if (probe == 0)
        return 0;

    uint32_t dir = probe->page_directory;

    /*
     * Unit checks must run atomically: task_create_user_program
     * returns with interrupts enabled, and the probe would
     * otherwise run (and exit) in the middle of them.
     */
    __asm__ volatile ("cli");

    /* Unit checks against the probe's own address space. */
    if (uaccess_check(dir, TASK_RING3_CODE_VA, 16, 0) != 0)
        goto fail;

    if (uaccess_check(dir, TASK_RING3_STACK_VA, 16, 1) != 0)
        goto fail;

    /* Code page is not writable. */
    if (uaccess_check(dir, TASK_RING3_CODE_VA, 16, 1) == 0)
        goto fail;

    /* Kernel address rejected. */
    if (uaccess_check(dir, 0x00100000U, 16, 0) == 0)
        goto fail;

    /* Kernel boundary rejected. */
    if (uaccess_check(dir, USER_ADDR_MAX, 4, 0) == 0)
        goto fail;

    /* Length wraparound rejected. */
    if (uaccess_check(dir, 0xFFFFFFF0U, 32, 0) == 0)
        goto fail;

    /* copy_to/from round-trip on the writable stack page. */
    {
        uint8_t pattern[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        uint8_t back[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        if (copy_to_user(UACCESS_GOOD_BUF, pattern, 8, dir) != 0)
            goto fail;

        if (copy_from_user(back, UACCESS_GOOD_BUF, 8, dir) != 0)
            goto fail;

        for (int i = 0; i < 8; i++) {
            if (back[i] != pattern[i])
                goto fail;
        }

        /* Copy into a kernel address must fail. */
        if (copy_to_user(0x00100000U, pattern, 8, dir) == 0)
            goto fail;
    }

    /* String copy: seed "hi" then read it back. */
    {
        char seed[3] = { 'h', 'i', 0 };
        char out[UACCESS_MAX_STRING];
        int n;

        if (copy_to_user(UACCESS_GOOD_BUF, seed, 3, dir) != 0)
            goto fail;

        n =
            copy_string_from_user(
                out,
                UACCESS_GOOD_BUF,
                sizeof(out),
                dir
            );

        if (n != 2 || out[0] != 'h' || out[1] != 'i' || out[2] != 0)
            goto fail;

        /* Unterminated string must fail, not overrun. */
        {
            uint8_t fill[8];
            uint32_t base = TASK_RING3_STACK_VA;

            for (int i = 0; i < 8; i++)
                fill[i] = 'A';

            /* Fill the first 64 bytes of the stack, no NUL. */
            for (uint32_t off = 0; off < 64; off += 8) {
                if (copy_to_user(base + off, fill, 8, dir) != 0)
                    goto fail;
            }

            if (copy_string_from_user(out, base, 64, dir) == 0)
                goto fail;
        }
    }

    /*
     * Live part: let the probe run in Ring 3. It performs a
     * bad-pointer SYS_GETTIME (must get -1) and a good one
     * (must get 0), records both, and exits.
     */
    __asm__ volatile ("sti");

    uint32_t start = timer_ticks;

    {
        const task_t *t;

        for (;;) {
            t = task_get((uint32_t)id);

            if (t == 0 || t->state == TASK_TERMINATED)
                break;

            if (timer_ticks - start > 500)
                break;

            __asm__ volatile ("hlt");
        }
    }

    __asm__ volatile ("cli");

    probe = task_get((uint32_t)id);

    if (probe == 0 || probe->state != TASK_TERMINATED)
        goto fail;

    if (uaccess_read_word(dir, UACCESS_RES_BAD) != 0xFFFFFFFFU)
        goto fail_cleanup;

    if (uaccess_read_word(dir, UACCESS_RES_GOOD) != 0U)
        goto fail_cleanup;

    task_wait((uint32_t)id);
    return 1;

fail_cleanup:
    task_wait((uint32_t)id);
    return 0;

fail:
    task_terminate((uint32_t)id);
    task_wait((uint32_t)id);
    return 0;
}
