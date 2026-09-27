#ifndef KMEM_H
#define KMEM_H

#include <stdint.h>

/*
 * Freestanding memory routines.
 *
 * The kernel is built with -ffreestanding -nostdlib, so the host's
 * <string.h> is not available: it pulls in hosted headers that do not
 * exist in a 32-bit freestanding environment. These are the few
 * primitives the kernel actually needs.
 *
 * All are byte-wise and do not assume alignment, so they are safe on
 * any buffer including unaligned disk staging areas.
 */

void *memset(void *dest, int value, uint32_t count);
void *memcpy(void *dest, const void *src, uint32_t count);

/* Copies count bytes, tolerating overlap, as memmove does. Used where
 * a source and destination may alias, such as removing an element
 * from an array in place. */
void *memmove(void *dest, const void *src, uint32_t count);

int memcmp(const void *a, const void *b, uint32_t count);

#endif
