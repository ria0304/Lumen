#include <stdint.h>
#include "frame.h"
#include "paging.h"
#include "console.h"

static uint32_t frame_bitmap[FRAME_BITMAP_WORDS];
static uint32_t free_frames;
static uint32_t used_frames;

/*
 * End of the contiguous reserved region covering low memory, the
 * kernel image, the master page directory/table and the legacy heap
 * reservation. Nothing below this is ever allocatable, so frame_free()
 * refuses it outright. Every reserve_range() call in frame_init() must
 * end at or below this value -- keeping the bound named here is what
 * stops the reservations and the free policy from drifting apart.
 */
#define FRAME_RESERVED_END 0x00210000U

static inline uint32_t frame_index(uint32_t address)
{
    return address / FRAME_SIZE;
}

static inline void mark_used(uint32_t index)
{
    frame_bitmap[index / 32U] |=
        1U << (index % 32U);
}

static inline void mark_free(uint32_t index)
{
    frame_bitmap[index / 32U] &=
        ~(1U << (index % 32U));
}

static inline int is_used(uint32_t index)
{
    return
        (frame_bitmap[index / 32U] &
         (1U << (index % 32U))) != 0;
}

static void reserve_range(
    uint32_t start,
    uint32_t end
)
{
    start &= ~(FRAME_SIZE - 1U);

    if (end & (FRAME_SIZE - 1U))
        end = (end + FRAME_SIZE - 1U) &
              ~(FRAME_SIZE - 1U);

    for (uint32_t address = start;
         address < end &&
         address < FRAME_MEMORY_LIMIT;
         address += FRAME_SIZE) {

        uint32_t index = frame_index(address);

        if (!is_used(index)) {
            mark_used(index);

            if (free_frames > 0)
                --free_frames;

            ++used_frames;
        }
    }
}

void frame_init(void)
{
    for (uint32_t i = 0;
         i < FRAME_BITMAP_WORDS;
         ++i) {

        frame_bitmap[i] = 0;
    }

    free_frames = FRAME_COUNT;
    used_frames = 0;

    /*
     * Reserve conventional/early memory.
     */
    reserve_range(
        0x00000000U,
        0x00100000U
    );

    /*
     * Kernel image, static page directory, and the first kernel page
     * table.
     *
     * The master page directory and its single page table sit at
     * 0x00100000/0x00101000 (PAGE_DIRECTORY_ADDRESS and
     * PAGE_TABLE_ADDRESS in kernel/paging.h), inside this range. An
     * earlier layout kept them at 0x00220000 while reserving
     * 0x00210000-0x00212000 instead, which left the real directory
     * allocatable -- a frame_alloc() for a Ring 3 page directory
     * could land on the very address CR3 was pointing at.
     *
     * The range is reserved wholesale rather than naming the two
     * pages so the free-policy check in frame_free() can stay a
     * single lower-bound comparison.
     */
    reserve_range(
        0x00100000U,
        0x00200000U
    );

    /*
     * Legacy heap reservation, and the end of the reserved region.
     * See FRAME_RESERVED_END: frame_free() refuses anything below
     * this address, so this range has to be the last thing reserved.
     */
    reserve_range(
        0x00200000U,
        FRAME_RESERVED_END
    );

    console_info(
        "Frame allocator initialized: OK"
    );
}

uint32_t frame_alloc(void)
{
    for (uint32_t index = 0;
         index < FRAME_COUNT;
         ++index) {

        if (is_used(index))
            continue;

        mark_used(index);

        if (free_frames > 0)
            --free_frames;

        ++used_frames;

        return index * FRAME_SIZE;
    }

    return FRAME_INVALID;
}

/*
 * Allocate 'count' physically contiguous frames and return the base
 * physical address, or FRAME_INVALID.
 *
 * DMA descriptors and the RTL8139 receive ring must live in a single
 * unbroken physical range: the hardware walks them by arithmetic on
 * the base address, so individually allocated pages will not do.
 * Everything is released again if the range cannot be completed.
 */
uint32_t frame_alloc_contiguous(uint32_t count)
{
    if (count == 0 || count > FRAME_COUNT)
        return FRAME_INVALID;

    for (uint32_t index = 0; index + count <= FRAME_COUNT; ++index) {

        uint32_t i;
        int run = 1;

        for (i = 0; i < count; ++i) {
            if (is_used(index + i)) {
                run = 0;
                break;
            }
        }

        if (!run)
            continue;

        for (i = 0; i < count; ++i) {
            mark_used(index + i);

            if (free_frames > 0)
                --free_frames;

            ++used_frames;
        }

        return index * FRAME_SIZE;
    }

    return FRAME_INVALID;
}

int frame_free_contiguous(uint32_t physical_address, uint32_t count)
{
    if (count == 0)
        return -1;

    if ((physical_address & (FRAME_SIZE - 1U)) != 0)
        return -1;

    uint32_t index = physical_address / FRAME_SIZE;

    if (index + count > FRAME_COUNT)
        return -1;

    for (uint32_t i = 0; i < count; ++i) {
        if (is_used(index + i))
            frame_free(physical_address + i * FRAME_SIZE);
    }

    return 0;
}

int frame_free(uint32_t physical_address)
{
    if ((physical_address &
         (FRAME_SIZE - 1U)) != 0)
        return -1;

    if (physical_address >= FRAME_MEMORY_LIMIT)
        return -1;

    /*
     * Never allow the allocator to release reserved
     * boot/kernel/heap/paging memory. The bound is the end of that
     * reserved region (FRAME_RESERVED_END), not a separate constant,
     * so it cannot drift out of step with frame_init().
     */
    if (physical_address < FRAME_RESERVED_END)
        return -1;

    uint32_t index =
        frame_index(physical_address);

    if (!is_used(index))
        return -1;

    mark_free(index);

    ++free_frames;

    if (used_frames > 0)
        --used_frames;

    return 0;
}

uint32_t frame_free_count(void)
{
    return free_frames;
}

uint32_t frame_used_count(void)
{
    return used_frames;
}

int frame_is_free(uint32_t physical_address)
{
    if ((physical_address &
         (FRAME_SIZE - 1U)) != 0)
        return 0;

    if (physical_address >= FRAME_MEMORY_LIMIT)
        return 0;

    return !is_used(
        frame_index(physical_address)
    );
}

int frame_run_self_test(void)
{
    uint32_t before =
        frame_free_count();

    uint32_t first =
        frame_alloc();

    uint32_t second =
        frame_alloc();

    if (first == FRAME_INVALID ||
        second == FRAME_INVALID) {
        console_error("Frame allocator test: out of frames");
        return 0;
    }

    if (first == second) {
        console_error("Frame allocator test: two allocs returned the same frame");
        return 0;
    }

    if ((first & (FRAME_SIZE - 1U)) != 0 ||
        (second & (FRAME_SIZE - 1U)) != 0) {
        console_error("Frame allocator test: misaligned frame returned");
        return 0;
    }

    if (frame_is_free(first) ||
        frame_is_free(second)) {
        console_error("Frame allocator test: allocated frame still reports free");
        return 0;
    }

    if (frame_free(first) != 0) {
        console_error("Frame allocator test: freeing the first frame failed");
        return 0;
    }

    if (frame_free(second) != 0) {
        console_error("Frame allocator test: freeing the second frame failed");
        return 0;
    }

    if (frame_free_count() != before) {
        console_error("Frame allocator test: free count did not return to its starting value");
        return 0;
    }

    if (frame_free(0x12345U) == 0) {
        console_error("Frame allocator test: misaligned free was accepted");
        return 0;
    }

    /*
     * The master page directory must never be allocatable or
     * freeable: it is the table CR3 is actively translating through.
     */
    if (frame_is_free(PAGE_DIRECTORY_ADDRESS)) {
        console_error("Frame allocator test: page directory is allocatable");
        return 0;
    }

    if (frame_free(PAGE_DIRECTORY_ADDRESS) == 0) {
        console_error("Frame allocator test: freeing the page directory was accepted");
        return 0;
    }

    if (frame_is_free(PAGE_TABLE_ADDRESS)) {
        console_error("Frame allocator test: page table is allocatable");
        return 0;
    }

    if (frame_free(PAGE_TABLE_ADDRESS) == 0) {
        console_error("Frame allocator test: freeing the page table was accepted");
        return 0;
    }

    console_info(
        "Frame allocator test: PASS"
    );

    return 1;
}
