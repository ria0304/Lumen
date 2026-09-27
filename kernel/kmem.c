#include <stdint.h>
#include "kmem.h"

void *memset(void *dest, int value, uint32_t count)
{
    uint8_t *p = (uint8_t *)dest;
    uint8_t v = (uint8_t)value;

    /* Fill a word at a time once the destination is aligned, since the
     * compiler can turn that into a single wide store. The scalar loop
     * covers the head, any unaligned middle, and the tail. */
    while (count > 0 && ((uintptr_t)p & 3) != 0) {
        *p++ = v;
        count--;
    }

    while (count >= 4) {
        uint32_t word = (uint32_t)v;

        word |= word << 8;
        word |= word << 16;

        *(uint32_t *)p = word;
        p += 4;
        count -= 4;
    }

    while (count > 0) {
        *p++ = v;
        count--;
    }

    return dest;
}

void *memcpy(void *dest, const void *src, uint32_t count)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    while (count >= 4 && ((uintptr_t)d & 3) == 0 && ((uintptr_t)s & 3) == 0) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
        count -= 4;
    }

    while (count > 0) {
        *d++ = *s++;
        count--;
    }

    return dest;
}

void *memmove(void *dest, const void *src, uint32_t count)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || count == 0)
        return dest;

    /* Copy backwards when the destination is above the source,
     * otherwise a forward copy would overwrite bytes it has yet to
     * read. */
    if (d > s) {
        d += count;
        s += count;

        while (count > 0) {
            *--d = *--s;
            count--;
        }
    } else {
        while (count > 0) {
            *d++ = *s++;
            count--;
        }
    }

    return dest;
}

int memcmp(const void *a, const void *b, uint32_t count)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    for (uint32_t i = 0; i < count; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }

    return 0;
}
