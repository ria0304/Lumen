#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

void scheduler_init(void);

uint32_t scheduler_current_task(void);
uint32_t scheduler_next_task(void);

void scheduler_tick(void);

/*
 * Called directly by IRQ0.
 *
 * Receives the address of the PUSHA frame and returns
 * the stack address that IRQ0 must restore.
 */
uint32_t scheduler_irq(uint32_t *frame);

#endif
