#include "syscall.h"
#include "console.h"

void syscall_handler(void)
{
    /*
     * First Ring-3 syscall milestone.
     *
     * The user program places:
     *
     *     EAX = 1
     *
     * and executes:
     *
     *     int 0x80
     *
     * The ISR currently preserves registers with PUSHA,
     * calls this function, then IRETs back to user mode.
     */
    console_info("Ring 3 system call received: PASS");
}
