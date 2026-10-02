#ifndef RING3TEST_H
#define RING3TEST_H

/*
 * End-to-end Ring 3 test. Creates a real CPL 3 task running
 * kernel/ring3test.asm, lets it do file I/O through the syscall
 * table, and then checks both what the task reported and what
 * actually landed on the filesystem.
 *
 * This needs a mounted filesystem, so it is reported separately from
 * the other self-tests rather than from the syscall one (which runs
 * before the disk is available).
 */
int ring3_io_run_self_test(void);

/*
 * Fault isolation: a Ring 3 fault must retire only the offending
 * task. Two programs are made to fault (unmapped read, and a write to
 * a read-only page), then a healthy task is required to still run.
 */
int ring3_fault_run_self_test(void);

/* fork() from Ring 3: clone, child return value, parent/child link. */
int ring3_fork_run_self_test(void);

/* exec(): replace a Ring 3 image from LumenFS and run the result. */
int ring3_exec_run_self_test(void);

/* Syscall coverage, especially the dispatcher error paths. */
int ring3_syscalls_run_self_test(void);

/* The ELF loader: a real ELF staged on LumenFS, run and verified. */
int ring3_elf_run_self_test(void);

#endif