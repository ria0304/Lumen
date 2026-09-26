#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define FRAME_SIZE              4096U
#define FRAME_MEMORY_LIMIT      0x01000000U
#define FRAME_COUNT             (FRAME_MEMORY_LIMIT / FRAME_SIZE)
#define FRAME_BITMAP_WORDS      ((FRAME_COUNT + 31U) / 32U)
#define FRAME_INVALID            0xFFFFFFFFU

void frame_init(void);
uint32_t frame_alloc(void);
int frame_free(uint32_t physical_address);
uint32_t frame_free_count(void);
uint32_t frame_used_count(void);
int frame_is_free(uint32_t physical_address);
int frame_run_self_test(void);

#endif
