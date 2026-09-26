#ifndef RING3_H
#define RING3_H

#include <stdint.h>

#define RING3_CODE_VA   0x00C00000U
#define RING3_STACK_VA  0x00C01000U
#define RING3_STACK_TOP 0x00C02000U

int ring3_init(void);
int ring3_run_test(void);

#endif
