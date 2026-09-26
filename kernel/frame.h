#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define FRAME_SIZE 4096U

/*
 * Lumer currently manages physical memory below 16 MiB.
 *
 * 16 MiB / 4 KiB = 4096 frames.
 */
#define FRAME_MEMORY_LIMIT 0x01000000U
#define FRAME_COUNT        (FRAME_MEMORY_LIMIT / FRAME_SIZE)
#define FRAME_BITMAP_WORDS ((FRAME_COUNT + 31U) / 32U)

/*
 * Initialize the physical-frame allocator.
 */
void frame_init(void);

/*
 * Allocate one physical 4 KiB frame.
 *
 * Returns the physical address of the frame,
 * or 0xFFFFFFFF on failure.
 */
uint32_t frame_alloc(void);

/*
 * Free a previously allocated physical frame.
 *
 * Returns 0 on success, -1 on invalid input.
 */
int frame_free(uint32_t physical_address);

/*
 * Query allocator state.
 */
uint32_t frame_free_count(void);
uint32_t frame_used_count(void);
int frame_is_free(uint32_t physical_address);
int frame_run_self_test(void);

#endif
