#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>

void heap_init(void);
void *kmalloc(uint32_t size);
uint32_t heap_used(void);

#endif
