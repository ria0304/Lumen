#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "privilege.h"

#define MAX_TASKS 16
#define TASK_STACK_SIZE 4096

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
} task_state_t;

typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t eip;
    uint32_t eflags;
} task_context_t;

typedef struct {
    uint32_t id;
    task_state_t state;
    uint32_t privilege;

    uint32_t stack_base;
    uint32_t stack_size;

    /*
     * Address of the saved interrupt frame.
     *
     * Layout:
     *   EDI
     *   ESI
     *   EBP
     *   ESP (ignored by POPA)
     *   EBX
     *   EDX
     *   ECX
     *   EAX
     *   EIP
     *   CS
     *   EFLAGS
     */
    uint32_t switch_esp;

    task_context_t context;
} task_t;

void task_init(void);

int task_create(void);
int task_create_with_privilege(uint32_t privilege);
int task_terminate(uint32_t id);

const task_t *task_get(uint32_t id);
uint32_t task_count(void);

#endif
