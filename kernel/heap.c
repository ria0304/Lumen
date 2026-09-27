#include <stdint.h>

#include "heap.h"
#include "paging.h"
#include "console.h"
#include "lock.h"

#define HEAP_ALIGN 8U
#define HEAP_MAGIC 0x48454150U

#define BLOCK_FREE 1U
#define BLOCK_USED 0U

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint32_t free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_head;

static uint32_t heap_brk;
static uint32_t heap_used_bytes;
static uint32_t heap_committed_bytes;

static uint32_t align_up(uint32_t value)
{
    return
        (value + HEAP_ALIGN - 1U) &
        ~(HEAP_ALIGN - 1U);
}

static int heap_grow_to(
    uint32_t end_address
)
{
    if (end_address > HEAP_END)
        return -1;

    while (heap_brk < end_address) {

        if (paging_alloc_zero_page(
                heap_brk,
                PAGE_WRITE) != 0)
            return -1;

        heap_brk += HEAP_PAGE_SIZE;
        heap_committed_bytes +=
            HEAP_PAGE_SIZE;
    }

    return 0;
}

static heap_block_t *find_free(
    uint32_t size
)
{
    heap_block_t *block =
        heap_head;

    while (block) {

        if (block->free &&
            block->size >= size)
            return block;

        block = block->next;
    }

    return 0;
}

static void split_block(
    heap_block_t *block,
    uint32_t size
)
{
    uint32_t header =
        align_up(sizeof(heap_block_t));

    if (block->size <
        size + header + HEAP_ALIGN)
        return;

    heap_block_t *rest =
        (heap_block_t *)
        ((uint8_t *)block +
         header +
         size);

    rest->magic = HEAP_MAGIC;
    rest->size =
        block->size -
        size -
        header;

    rest->free = BLOCK_FREE;

    rest->next =
        block->next;

    rest->prev =
        block;

    if (rest->next)
        rest->next->prev =
            rest;

    block->next =
        rest;

    block->size =
        size;
}

static void coalesce(
    heap_block_t *block
)
{
    uint32_t header =
        align_up(sizeof(heap_block_t));

    if (block->next &&
        block->next->free) {

        heap_block_t *next =
            block->next;

        block->size +=
            header +
            next->size;

        block->next =
            next->next;

        if (block->next)
            block->next->prev =
                block;
    }

    if (block->prev &&
        block->prev->free) {

        heap_block_t *prev =
            block->prev;

        prev->size +=
            header +
            block->size;

        prev->next =
            block->next;

        if (prev->next)
            prev->next->prev =
                prev;
    }
}

void heap_init(void)
{
    heap_head = 0;

    heap_brk =
        HEAP_START;

    heap_used_bytes = 0;
    heap_committed_bytes = 0;

    if (heap_grow_to(
            HEAP_START +
            HEAP_PAGE_SIZE) != 0) {

        console_error(
            "Heap: initial page allocation FAILED"
        );

        return;
    }

    heap_head =
        (heap_block_t *)HEAP_START;

    heap_head->magic =
        HEAP_MAGIC;

    heap_head->size =
        HEAP_PAGE_SIZE -
        align_up(sizeof(heap_block_t));

    heap_head->free =
        BLOCK_FREE;

    heap_head->next = 0;
    heap_head->prev = 0;
}

/*
 * The body of kmalloc(), split out so krealloc() can call it without
 * re-entering the interrupt-disable that the public wrapper already
 * holds. See lock.h for why disabling interrupts is sufficient
 * exclusion on this single-processor kernel.
 */
static void *heap_alloc_locked(uint32_t size)
{
    if (size == 0)
        return 0;

    size = align_up(size);

    uint32_t header =
        align_up(sizeof(heap_block_t));

    heap_block_t *block =
        find_free(size);

    if (!block) {

        uint32_t old_brk =
            heap_brk;

        uint32_t needed =
            header + size;

        if (heap_grow_to(
                heap_brk +
                needed) != 0)
            return 0;

        block =
            (heap_block_t *)old_brk;

        block->magic =
            HEAP_MAGIC;

        block->size =
            size;

        block->free =
            BLOCK_FREE;

        block->next = 0;
        block->prev = 0;

        if (!heap_head) {

            heap_head =
                block;

        } else {

            heap_block_t *tail =
                heap_head;

            while (tail->next)
                tail = tail->next;

            tail->next =
                block;

            block->prev =
                tail;
        }
    }

    split_block(
        block,
        size
    );

    block->free =
        BLOCK_USED;

    heap_used_bytes +=
        block->size;

    return
        (uint8_t *)block +
        header;
}

void *kmalloc(uint32_t size)
{
    irqflags_t flags =
        irq_save_disable();

    void *result =
        heap_alloc_locked(size);

    irq_restore(flags);

    return result;
}

/*
 * The body of kfree(), split for the same reason as
 * heap_alloc_locked().
 */
static void heap_free_locked(void *pointer)
{
    if (!pointer)
        return;

    uint32_t header =
        align_up(sizeof(heap_block_t));

    uint32_t address =
        (uint32_t)pointer;

    if (address <
        HEAP_START + header)
        return;

    if (address >= HEAP_END)
        return;

    heap_block_t *block =
        (heap_block_t *)
        ((uint8_t *)pointer -
         header);

    if (block->magic !=
        HEAP_MAGIC)
        return;

    if (block->free)
        return;

    block->free =
        BLOCK_FREE;

    if (heap_used_bytes >=
        block->size)
        heap_used_bytes -=
            block->size;
    else
        heap_used_bytes = 0;

    coalesce(block);
}

void kfree(void *pointer)
{
    irqflags_t flags =
        irq_save_disable();

    heap_free_locked(pointer);

    irq_restore(flags);
}

void *krealloc(
    void *pointer,
    uint32_t size
)
{
    /*
     * One critical section for the whole operation: krealloc both
     * allocates and frees, so taking the lock per public call would
     * deadlock against itself.
     */
    irqflags_t flags =
        irq_save_disable();

    if (!pointer) {
        void *fresh =
            heap_alloc_locked(size);

        irq_restore(flags);
        return fresh;
    }

    if (size == 0) {
        heap_free_locked(pointer);
        irq_restore(flags);
        return 0;
    }

    uint32_t header =
        align_up(sizeof(heap_block_t));

    heap_block_t *block =
        (heap_block_t *)
        ((uint8_t *)pointer -
         header);

    if (block->magic !=
        HEAP_MAGIC) {
        irq_restore(flags);
        return 0;
    }

    if (block->free) {
        irq_restore(flags);
        return 0;
    }

    size = align_up(size);

    if (size <= block->size) {

        if (heap_used_bytes >=
            block->size)
            heap_used_bytes -=
                block->size;

        block->size =
            size;

        heap_used_bytes +=
            block->size;

        split_block(
            block,
            size
        );

        irq_restore(flags);
        return pointer;
    }

    void *replacement =
        heap_alloc_locked(size);

    if (!replacement) {
        irq_restore(flags);
        return 0;
    }

    uint32_t copy =
        block->size < size
            ? block->size
            : size;

    uint8_t *src =
        (uint8_t *)pointer;

    uint8_t *dst =
        (uint8_t *)replacement;

    for (uint32_t i = 0;
         i < copy;
         ++i)
        dst[i] = src[i];

    heap_free_locked(pointer);

    irq_restore(flags);

    return replacement;
}

uint32_t heap_used(void)
{
    irqflags_t flags =
        irq_save_disable();

    uint32_t value =
        heap_used_bytes;

    irq_restore(flags);

    return value;
}

uint32_t heap_free(void)
{
    irqflags_t flags =
        irq_save_disable();

    uint32_t value = 0;

    if (heap_committed_bytes >=
        heap_used_bytes)
        value =
            heap_committed_bytes -
            heap_used_bytes;

    irq_restore(flags);

    return value;
}

uint32_t heap_committed(void)
{
    irqflags_t flags =
        irq_save_disable();

    uint32_t value =
        heap_committed_bytes;

    irq_restore(flags);

    return value;
}

int heap_run_self_test(void)
{
    uint8_t *a =
        (uint8_t *)kmalloc(64);

    uint8_t *b =
        (uint8_t *)kmalloc(128);

    if (!a || !b)
        return 0;

    for (uint32_t i = 0;
         i < 64;
         ++i)
        a[i] =
            (uint8_t)i;

    for (uint32_t i = 0;
         i < 64;
         ++i) {

        if (a[i] !=
            (uint8_t)i)
            return 0;
    }

    kfree(a);

    uint8_t *c =
        (uint8_t *)kmalloc(32);

    if (!c)
        return 0;

    uint8_t *d =
        (uint8_t *)krealloc(
            b,
            256
        );

    if (!d)
        return 0;

    kfree(c);
    kfree(d);

    console_info(
        "Heap page-backed allocator test: PASS"
    );

    return 1;
}
