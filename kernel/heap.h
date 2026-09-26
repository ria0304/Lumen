#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>

#define HEAP_START        0x00400000U
#define HEAP_END          0x00800000U
#define HEAP_PAGE_SIZE    4096U

void heap_init(void);
void *kmalloc(uint32_t size);
void kfree(void *pointer);
void *krealloc(void *pointer, uint32_t size);

uint32_t heap_used(void);
uint32_t heap_free(void);
uint32_t heap_committed(void);

int heap_run_self_test(void);

#endif
