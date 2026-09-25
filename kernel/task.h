#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "privilege.h"

#define MAX_TASKS 16

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
} task_state_t;

typedef struct {
    uint32_t id;
    task_state_t state;
    uint32_t privilege;
} task_t;

void task_init(void);

int task_create(void);
int task_create_with_privilege(uint32_t privilege);
int task_terminate(uint32_t id);

const task_t *task_get(uint32_t id);
uint32_t task_count(void);

#endif
