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
    uint32_t parent_id;
    task_state_t state;
    uint32_t privilege;

    /*
     * Physical address of this task's page directory.
     * Kernel-ring tasks share PAGE_DIRECTORY_ADDRESS (the
     * single global kernel directory). Ring 3 tasks get their
     * own, created by paging_create_address_space().
     */
    uint32_t page_directory;

    uint32_t stack_base;
    uint32_t stack_size;

    uint32_t switch_esp;

    task_context_t context;
} task_t;

void task_init(void);

int task_create(void);
int task_create_with_privilege(uint32_t privilege);

/*
 * Creates a Ring 3 task whose code page is a copy of 'code'
 * (up to PAGE_SIZE bytes -- one page, same limit the legacy
 * 'taskuser' path already has). Used by the loader to run a
 * program read from the filesystem instead of the fixed
 * ring3_test_program. Returns the new task's id, or -1 if
 * code_size exceeds a page or allocation fails.
 */
int task_create_user_program(const uint8_t *code, uint32_t code_size);

int task_terminate(uint32_t id);
int task_block(uint32_t id);
int task_wake(uint32_t id);
int task_yield(void);
int task_exit(void);
int task_wait(uint32_t child_id);

const task_t *task_get(uint32_t id);
uint32_t task_count(void);

int task_run_self_test(void);

#endif
