#include <stdint.h>

/*
 * Every kernel task begins here.
 *
 * HLT puts the CPU into an idle state until the
 * next interrupt. IRQ0 then preempts the task and
 * the scheduler can restore another task.
 */
void task_demo_entry(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
