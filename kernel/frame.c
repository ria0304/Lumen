#include <stdint.h>
#include "frame.h"
#include "console.h"

/*
 * One bit per physical 4 KiB frame.
 *
 * 0 = free
 * 1 = reserved/used
 */
static uint32_t frame_bitmap[FRAME_BITMAP_WORDS];

static uint32_t free_frames = 0;
static uint32_t used_frames = 0;

static inline uint32_t frame_index(uint32_t address)
{
    return address / FRAME_SIZE;
}

static inline void frame_mark_used(uint32_t index)
{
    frame_bitmap[index / 32U] |=
        (1U << (index % 32U));
}

static inline void frame_mark_free(uint32_t index)
{
    frame_bitmap[index / 32U] &=
        ~(1U << (index % 32U));
}

static inline int frame_bit_is_set(uint32_t index)
{
    return
        (frame_bitmap[index / 32U] &
         (1U << (index % 32U))) != 0;
}

static void frame_reserve_range(
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

        if (!frame_bit_is_set(index)) {
            frame_mark_used(index);

            if (free_frames > 0)
                free_frames--;

            used_frames++;
        }
    }
}

void frame_init(void)
{
    /*
     * Start with every frame free.
     */
    for (uint32_t i = 0; i < FRAME_BITMAP_WORDS; i++)
        frame_bitmap[i] = 0;

    free_frames = FRAME_COUNT;
    used_frames = 0;

    /*
     * Reserve the first 1 MiB.
     *
     * This includes:
     * - real-mode/protected-mode low memory
     * - VGA memory
     * - BIOS-related conventional memory areas
     * - Lumer's early boot structures
     */
    frame_reserve_range(
        0x00000000U,
        0x00100000U
    );

    /*
     * Reserve the current kernel image region.
     *
     * The kernel is loaded at 0x10000. Reserve through
     * 0x200000 so the current kernel/early runtime region
     * cannot accidentally be returned as free RAM.
     */
    frame_reserve_range(
        0x00100000U,
        0x00200000U
    );

    /*
     * Reserve the current 64 KiB heap.
     */
    frame_reserve_range(
        0x00200000U,
        0x00210000U
    );

    /*
     * Reserve the gap between the heap and the page
     * directory/table region below. frame_free() treats
     * everything under 0x00222000 as permanently reserved,
     * so this range must actually be marked used or
     * frame_alloc() will hand out a frame here that
     * frame_free() then refuses to release.
     */
    frame_reserve_range(
        0x00210000U,
        0x00220000U
    );

    /*
     * Reserve page directory + page table.
     */
    frame_reserve_range(
        0x00220000U,
        0x00222000U
    );

    console_info("Frame allocator initialized: OK");
}

uint32_t frame_alloc(void)
{
    for (uint32_t index = 0;
         index < FRAME_COUNT;
         index++) {

        if (frame_bit_is_set(index))
            continue;

        frame_mark_used(index);

        if (free_frames > 0)
            free_frames--;

        used_frames++;

        return index * FRAME_SIZE;
    }

    return 0xFFFFFFFFU;
}

int frame_free(uint32_t physical_address)
{
    /*
     * Frame addresses must be page aligned.
     */
    if ((physical_address &
         (FRAME_SIZE - 1U)) != 0)
        return -1;

    if (physical_address >= FRAME_MEMORY_LIMIT)
        return -1;

    uint32_t index =
        frame_index(physical_address);

    /*
     * Do not free a frame that is already free.
     */
    if (!frame_bit_is_set(index))
        return -1;

    /*
     * Do not allow the permanently reserved low-memory
     * region to be returned to the allocator.
     */
    if (physical_address < 0x00222000U)
        return -1;

    frame_mark_free(index);

    if (used_frames > 0)
        used_frames--;

    free_frames++;

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

    return !frame_bit_is_set(
        frame_index(physical_address)
    );
}

static int frame_self_test(void)
{
    uint32_t before = frame_free_count();

    uint32_t first = frame_alloc();

    if (first == 0xFFFFFFFFU) {
        console_info("Frame test 1: alloc FAILED");
        return 0;
    }

    uint32_t second = frame_alloc();

    if (second == 0xFFFFFFFFU) {
        console_info("Frame test 2: second alloc FAILED");
        frame_free(first);
        return 0;
    }

    if (first == second) {
        console_info("Frame test 3: duplicate frame");
        frame_free(first);
        frame_free(second);
        return 0;
    }

    if ((first & (FRAME_SIZE - 1U)) != 0) {
        console_info("Frame test 4: first alignment FAILED");
        frame_free(first);
        frame_free(second);
        return 0;
    }

    if ((second & (FRAME_SIZE - 1U)) != 0) {
        console_info("Frame test 5: second alignment FAILED");
        frame_free(first);
        frame_free(second);
        return 0;
    }

    if (frame_is_free(first)) {
        console_info("Frame test 6: allocated frame marked free");
        frame_free(first);
        frame_free(second);
        return 0;
    }

    if (frame_free(first) != 0) {
        console_info("Frame test 7: first free FAILED");
        frame_free(second);
        return 0;
    }

    if (frame_free(second) != 0) {
        console_info("Frame test 8: second free FAILED");
        return 0;
    }

    if (frame_free_count() != before) {
        console_info("Frame test 9: count mismatch");
        return 0;
    }

    if (frame_free(0x12345U) == 0) {
        console_info("Frame test 10: unaligned free accepted");
        return 0;
    }

    if (frame_free(0x00100000U) == 0) {
        console_info("Frame test 11: reserved frame accepted");
        return 0;
    }

    console_info("Frame test checks: ALL PASS");
    return 1;
}

int frame_run_self_test(void)
{
    return frame_self_test();
}
