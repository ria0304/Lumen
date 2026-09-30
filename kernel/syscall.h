#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_GETPID     1U
#define SYS_YIELD      2U
#define SYS_EXIT       3U
#define SYS_WAIT       4U
#define SYS_FORK       5U
#define SYS_EXEC       6U
#define SYS_GETPPID    7U
#define SYS_GETUID     8U
#define SYS_GETGID     9U
#define SYS_SETSID    10U
#define SYS_GETPGID   11U
#define SYS_KILL      12U
#define SYS_SIGNAL    13U
#define SYS_SIGPROCMASK 14U
#define SYS_PIPE      15U
#define SYS_DUP2      16U
#define SYS_CLOSE     17U
#define SYS_OPEN      18U
#define SYS_READ      19U
#define SYS_WRITE     20U
#define SYS_GETTIME     21U
#define SYS_REBOOT     22U
#define SYS_SOCKET     23U
#define SYS_SEND       24U
#define SYS_RECV       25U

void syscall_handler(uint32_t *frame);

#endif
