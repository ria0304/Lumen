#include <stdint.h>

#include "syscall.h"
#include "task.h"
#include "scheduler.h"
#include "console.h"

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

        default:
            frame[7] = 0xFFFFFFFFU;
            console_error(
                "Unknown system call"
            );
            break;
    }
}
