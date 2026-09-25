#include <stdint.h>

/*
 * Simple bump allocator for Lumer.
 *
 * Allocations are sequential and are not freed.
 * This is intentionally simple for the first memory-management layer.
 */

#define HEAP_START 0x200000
#define HEAP_SIZE  0x10000

static uintptr_t heap_current = HEAP_START;
static uintptr_t heap_end = HEAP_START + HEAP_SIZE;

void heap_init(void)
{
    heap_current = HEAP_START;
}

void *kmalloc(uint32_t size)
{
    if (size == 0)
        return 0;

    /* Align allocations to 4 bytes. */
    size = (size + 3) & ~3U;

    if (heap_current + size > heap_end)
        return 0;

    void *address = (void *)heap_current;
    heap_current += size;

    return address;
}

uint32_t heap_used(void)
{
    return (uint32_t)(heap_current - HEAP_START);
}
