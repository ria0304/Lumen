#include <stdint.h>
#include "frame.h"
#include "console.h"

static uint32_t frame_bitmap[FRAME_BITMAP_WORDS];
static uint32_t free_frames;
static uint32_t used_frames;

static inline uint32_t frame_index(uint32_t address)
{
    return address / FRAME_SIZE;
}

static inline void mark_used(uint32_t index)
{
    frame_bitmap[index / 32U] |= 1U << (index % 32U);
}

static inline void mark_free(uint32_t index)
{
    frame_bitmap[index / 32U] &= ~(1U << (index % 32U));
}

static inline int is_used(uint32_t index)
{
    return (frame_bitmap[index / 32U] &
            (1U << (index % 32U))) != 0;
}

static void reserve_range(uint32_t start, uint32_t end)
{
    start &= ~(FRAME_SIZE - 1U);
    end = (end + FRAME_SIZE - 1U) & ~(FRAME_SIZE - 1U);

    for (uint32_t address = start;
         address < end && address < FRAME_MEMORY_LIMIT;
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
    for (uint32_t i = 0; i < FRAME_BITMAP_WORDS; ++i)
        frame_bitmap[i] = 0;

    free_frames = FRAME_COUNT;
    used_frames = 0;

    reserve_range(0x00000000U, 0x00100000U);

    reserve_range(0x00100000U, 0x00200000U);

    /*
     * Keep the old heap/page-table region reserved.
     * This prevents old code or stale assumptions from
     * ever receiving these frames.
     */
    reserve_range(0x00200000U, 0x00220000U);

    reserve_range(0x00220000U, 0x00222000U);

    console_info("Frame allocator initialized: OK");
}

uint32_t frame_alloc(void)
{
    for (uint32_t index = 0;
         index < FRAME_COUNT;
         ++index) {

        if (is_used(index))
            continue;

        mark_used(index);
        --free_frames;
        ++used_frames;

        return index * FRAME_SIZE;
    }

    return FRAME_INVALID;
}

int frame_free(uint32_t physical_address)
{
    if ((physical_address & (FRAME_SIZE - 1U)) != 0)
        return -1;

    if (physical_address >= FRAME_MEMORY_LIMIT)
        return -1;

    if (physical_address < 0x00222000U)
        return -1;

    uint32_t index = frame_index(physical_address);

    if (!is_used(index))
        return -1;

    mark_free(index);

    ++free_frames;
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
    if ((physical_address & (FRAME_SIZE - 1U)) != 0)
        return 0;

    if (physical_address >= FRAME_MEMORY_LIMIT)
        return 0;

    return !is_used(frame_index(physical_address));
}

int frame_run_self_test(void)
{
    uint32_t before = frame_free_count();

    uint32_t a = frame_alloc();
    uint32_t b = frame_alloc();

    if (a == FRAME_INVALID ||
        b == FRAME_INVALID ||
        a == b)
        return 0;

    if ((a & (FRAME_SIZE - 1U)) ||
        (b & (FRAME_SIZE - 1U)))
        return 0;

    if (frame_is_free(a) ||
        frame_is_free(b))
        return 0;

    if (frame_free(a) != 0 ||
        frame_free(b) != 0)
        return 0;

    if (frame_free_count() != before)
        return 0;

    if (frame_free(0x12345U) == 0)
        return 0;

    if (frame_free(0x00100000U) == 0)
        return 0;

    console_info("Frame allocator test: PASS");

    return 1;
}
