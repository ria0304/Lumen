#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

void scheduler_init(void);

uint32_t scheduler_current_task(void);
uint32_t scheduler_next_task(void);

void scheduler_tick(void);

uint32_t scheduler_irq(uint32_t *frame);

/*
 * Immediately switch away from the current task after SYS_EXIT.
 * The supplied frame belongs to the exiting task and must not be
 * returned through iret.
 */
uint32_t scheduler_exit_current(uint32_t *frame);

int scheduler_run_self_test(void);

#endif
