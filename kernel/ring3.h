#ifndef RING3_H
#define RING3_H

#include <stdint.h>

/*
 * Legacy single-instance Ring 3 demo, used by the 'usermode'
 * shell command. Maps into whichever directory is active at the
 * time (the master kernel directory, since this always runs from
 * task 0).
 */
#define RING3_CODE_VA   0x00C00000U
#define RING3_STACK_VA  0x00C01000U
#define RING3_STACK_TOP 0x00C02000U

/*
 * Per-task Ring 3 address space, used by task_create_with_privilege
 * (the 'taskuser' shell command). Deliberately a different
 * directory index than the legacy addresses above, so a task's
 * private page table can never alias the legacy mapping's table
 * if both features are used in the same session.
 */
#define TASK_RING3_CODE_VA   0x01000000U
#define TASK_RING3_STACK_VA  0x01001000U
#define TASK_RING3_STACK_TOP 0x01002000U

extern const uint8_t ring3_test_program[];
extern const uint32_t ring3_test_program_size;

extern const uint8_t ring3_exit_program[];
extern const uint32_t ring3_exit_program_size;

int ring3_init(void);
int ring3_run_test(void);

#endif
