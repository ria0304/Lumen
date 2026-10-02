#include <stdint.h>

#include "syscall.h"
#include "task.h"
#include "scheduler.h"
#include "console.h"
#include "rtc.h"
#include "net.h"
#include "uaccess.h"

void syscall_handler(uint32_t *frame)
{
    if (frame == 0)
        return;

    /*
     * PUSHA layout:
     *
     * frame[0] = EDI
     * frame[1] = ESI
     * frame[2] = EBP
     * frame[3] = ESP
     * frame[4] = EBX
     * frame[5] = EDX
     * frame[6] = ECX
     * frame[7] = EAX
     */
    uint32_t syscall_number =
        frame[7];

    /*
     * Pointer validation: a Ring 3 caller may only pass
     * pointers into its own user-mapped pages below the
     * kernel boundary. Ring 0 callers are trusted kernel
     * code and skip the check.
     */
    const task_t *caller = task_get(scheduler_current_task());
    int caller_is_user =
        caller != 0 && caller->privilege == USER_RING;
    uint32_t caller_dir =
        caller != 0 ? caller->page_directory : 0;

    switch (syscall_number) {

        case SYS_GETPID:
        {
            uint32_t id =
                scheduler_current_task();

            frame[7] = id;

            terminal_write(
                "[INFO] Syscall GETPID: task "
            );

            char digits[10];
            int count = 0;
            uint32_t value = id;

            if (value == 0) {
                terminal_putchar('0');
            } else {
                while (value > 0) {
                    digits[count++] =
                        '0' + (value % 10);
                    value /= 10;
                }

                while (count > 0)
                    terminal_putchar(
                        digits[--count]
                    );
            }

            terminal_putchar('\n');
            break;
        }

        case SYS_YIELD:
        {
            frame[7] =
                (task_yield() == 0) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_EXIT:
        {
            /*
             * task_exit() marks the current process terminated.
             * The timer interrupt will select another runnable
             * task and the terminated task will never be scheduled
             * again until its parent collects the slot.
             */
            frame[7] =
                (task_exit() == 0) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_WAIT:
        {
            /*
             * EAX = SYS_WAIT
             * EBX = child PID
             */
            uint32_t child_id = frame[4];

            frame[7] =
                (task_wait(child_id) >= 0)
                    ? child_id
                    : 0xFFFFFFFFU;
            break;
        }

        case SYS_GETPPID:
        {
            uint32_t id = scheduler_current_task();
            const task_t *task = task_get(id);
            if (task != 0)
                frame[7] = task->parent_id;
            else
                frame[7] = 0xFFFFFFFFU;
            break;
        }

        case SYS_GETUID:
        case SYS_GETGID:
        {
            uint32_t id = scheduler_current_task();
            const task_t *task = task_get(id);
            if (task != 0 && task->privilege == USER_RING)
                frame[7] = 0;  /* root for now */
            else
                frame[7] = 0xFFFFFFFFU;
            break;
        }

        case SYS_SETSID:
        {
            uint32_t id = scheduler_current_task();
            task_t *task = (task_t *)task_get(id);
            if (task != 0) {
                task->parent_id = 0;  /* session leader */
                frame[7] = id;
            } else {
                frame[7] = 0xFFFFFFFFU;
            }
            break;
        }

        case SYS_GETPGID:
        {
            uint32_t id = scheduler_current_task();
            const task_t *task = task_get(id);
            if (task != 0)
                frame[7] = id;  /* process group = pid for now */
            else
                frame[7] = 0xFFFFFFFFU;
            break;
        }

        case SYS_FORK:
        {
            /*
             * A Ring 3 caller needs task_fork_user(), which clones
             * the address space and hands the child a copy of this
             * frame so it resumes after the syscall with EAX = 0.
             * task_fork() is for kernel-ring callers and deliberately
             * refuses a user task.
             */
            int child =
                caller_is_user
                    ? task_fork_user(frame)
                    : task_fork();

            frame[7] = (child >= 0) ? (uint32_t)child : 0xFFFFFFFFU;
            break;
        }

        case SYS_EXEC:
        {
            /*
             * EBX = filename pointer
             */
            uint32_t upath = frame[4];
            char kpath[UACCESS_MAX_STRING];

            if (caller_is_user &&
                copy_string_from_user(
                    kpath,
                    upath,
                    sizeof(kpath),
                    caller_dir
                ) < 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            const char *path =
                caller_is_user ? kpath : (const char *)upath;

            /* sys_exec rewrites the frame itself so the return path
             * enters the new image; on failure it leaves it alone and
             * we report the error as usual. */
            if (!sys_exec(path, frame))
                frame[7] = 0xFFFFFFFFU;
            break;
        }

        case SYS_KILL:
        {
            /*
             * EBX = pid
             * ECX = signal
             */
            uint32_t target_pid = frame[4];
            uint32_t sig = frame[5];
            frame[7] = sys_kill(target_pid, sig) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_SIGNAL:
        {
            /*
             * EBX = signal
             * ECX = handler
             */
            uint32_t sig = frame[4];
            uint32_t uhandler = frame[5];
            void (*handler)(int) = (void (*)(int))uhandler;

            if (caller_is_user &&
                uaccess_check(caller_dir, uhandler, 4, 0) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            frame[7] = sys_signal(sig, handler) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_SIGPROCMASK:
        {
            /*
             * EBX = how
             * ECX = mask
             */
            uint32_t how = frame[4];
            uint32_t mask = frame[5];
            frame[7] = sys_sigprocmask(how, mask);
            break;
        }

        case SYS_PIPE:
        {
            /*
             * EAX = pointer to array of 2 integers for file descriptors
             */
            uint32_t ufds = frame[4];
            int *fds = (int *)ufds;

            if (caller_is_user &&
                uaccess_check(
                    caller_dir,
                    ufds,
                    2 * sizeof(int),
                    1
                ) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            frame[7] = sys_pipe(fds) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_DUP2:
        {
            /*
             * EBX = oldfd
             * ECX = newfd
             */
            int oldfd = frame[4];
            int newfd = frame[5];
            frame[7] = sys_dup2(oldfd, newfd) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_CLOSE:
        {
            /*
             * EBX = fd
             */
            int fd = frame[4];
            frame[7] = sys_close(fd) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_OPEN:
        {
            uint32_t upath = frame[4];
            uint32_t flags = frame[5];
            char kpath[UACCESS_MAX_STRING];

            if (caller_is_user &&
                copy_string_from_user(
                    kpath,
                    upath,
                    sizeof(kpath),
                    caller_dir
                ) < 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            const char *path =
                caller_is_user ? kpath : (const char *)upath;

            int fd = sys_open(path, flags);
            frame[7] = (fd >= 0) ? (uint32_t)fd : 0xFFFFFFFFU;
            break;
        }

        case SYS_READ:
        {
            int fd = frame[4];
            uint32_t ubuf = frame[5];
            void *buf = (void *)ubuf;
            uint32_t len = frame[6];

            if (caller_is_user &&
                uaccess_check(caller_dir, ubuf, len, 1) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            int n = sys_read(fd, buf, len);
            frame[7] = (n >= 0) ? (uint32_t)n : 0xFFFFFFFFU;
            break;
        }

        case SYS_WRITE:
        {
            int fd = frame[4];
            uint32_t ubuf = frame[5];
            const void *buf = (const void *)ubuf;
            uint32_t len = frame[6];

            if (caller_is_user &&
                uaccess_check(caller_dir, ubuf, len, 0) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            int n = sys_write(fd, buf, len);
            frame[7] = (n >= 0) ? (uint32_t)n : 0xFFFFFFFFU;
            break;
        }

        case SYS_GETTIME:
        {
            uint32_t uout = frame[4];
            rtc_time_t *out = (rtc_time_t *)uout;

            if (caller_is_user &&
                uaccess_check(
                    caller_dir,
                    uout,
                    sizeof(rtc_time_t),
                    1
                ) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            frame[7] = (out && rtc_read(out) == 0) ? 0 : 0xFFFFFFFFU;
            break;
        }

        case SYS_REBOOT:
        {
            /* workers: triple-fault via IDT load of NULL, else halt */
            __asm__ volatile("cli; hlt");
            frame[7] = 0;
            break;
        }

        case SYS_SOCKET:
            frame[7] = 0; /* loopback fd 0 */
            break;

        case SYS_SEND:
        {
            uint32_t ubuf = frame[4];
            const void *buf = (const void *)ubuf;
            uint32_t len = frame[5];

            if (caller_is_user &&
                uaccess_check(caller_dir, ubuf, len, 0) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            int n = buf ? net_send(buf, len) : -1;
            frame[7] = (n >= 0) ? (uint32_t)n : 0xFFFFFFFFU;
            break;
        }

        case SYS_RECV:
        {
            uint32_t ubuf = frame[4];
            void *buf = (void *)ubuf;
            uint32_t len = frame[5];

            if (caller_is_user &&
                uaccess_check(caller_dir, ubuf, len, 1) != 0) {
                frame[7] = 0xFFFFFFFFU;
                break;
            }

            int n = buf ? net_recv(buf, len) : -1;
            frame[7] = (n >= 0) ? (uint32_t)n : 0xFFFFFFFFU;
            break;
        }

        default:
            frame[7] = 0xFFFFFFFFU;
            console_error(
                "Unknown system call"
            );
            break;
    }
}
