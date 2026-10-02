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

#endif