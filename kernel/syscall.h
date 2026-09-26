#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_GETPID 1U
#define SYS_YIELD  2U

void syscall_handler(uint32_t *frame);

#endif
