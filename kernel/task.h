#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "privilege.h"
#include "fs.h"
#include "elf.h"

#define MAX_TASKS 16
#define TASK_STACK_SIZE 4096
#define MAX_FDS 16

#define SIGKILL  9
#define SIGTERM 15
#define SIGCHLD 17

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
    fs_handle_t handles[MAX_FDS];
    int open[MAX_FDS];
} fd_table_t;

typedef struct {
    uint32_t pending;
    uint32_t mask;
    void (*handlers[32])(int);
} signal_t;

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

    fd_table_t fd_table;
    uint32_t cwd_inode;

    /*
     * Address space retired by exec(), pending reclaim by task_wait().
     * It cannot be freed inside sys_exec() because the CPU is still
     * running from it until the syscall frame is popped.
     */
    uint32_t reclaim_directory;

    signal_t signals;
} task_t;

void task_init(void);

int task_create(void);
int task_create_with_privilege(uint32_t privilege);

/*
 * Fork from a Ring 3 task. 'frame' is the interrupted syscall frame
 * the child must resume through; see the definition for why it is
 * required.
 */
int task_fork_user(uint32_t *frame);

/*
 * Creates a Ring 3 task whose code page is a copy of 'code'
 * (up to PAGE_SIZE bytes -- one page, same limit the legacy
 * 'taskuser' path already has). Used by the loader to run a
 * program read from the filesystem instead of the fixed
 * ring3_test_program. Returns the new task's id, or -1 if
 * code_size exceeds a page or allocation fails.
 */
int task_create_user_program(const uint8_t *code, uint32_t code_size);

/* Create a Ring 3 task from an ELF executable image.
 * This parses the ELF headers and maps each PT_LOAD segment
 * into the task's address space with appropriate permissions. */
int task_create_user_elf(const Elf32_Ehdr *ehdr, const void *buffer, uint32_t size);

int task_fork(void);
/*
 * Replace this task's image with 'filename' and enter it at Ring 3.
 * Rewrites the live syscall frame 'frame' so the syscall return path
 * lands in the new program. Returns 1 on success, 0 on failure.
 */
int sys_exec(const char *filename, uint32_t *frame);

int sys_kill(uint32_t pid, int sig);
int sys_signal(int sig, void (*handler)(int));
int sys_sigprocmask(int how, uint32_t mask);
int sys_pipe(int *fds);
int sys_dup2(int oldfd, int newfd);
int sys_close(int fd);
int sys_open(const char *path, uint32_t flags);
int sys_read(int fd, void *buf, uint32_t len);
int sys_write(int fd, const void *buf, uint32_t len);

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
