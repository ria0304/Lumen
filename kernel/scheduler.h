#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

void scheduler_init(void);

uint32_t scheduler_current_task(void);
uint32_t scheduler_next_task(void);

void scheduler_tick(void);

uint32_t scheduler_irq(uint32_t *frame);

int scheduler_run_self_test(void);

#endif
