#include <stdint.h>

#include "syscall.h"
#include "console.h"

void syscall_handler(void)
{
    uint32_t eax;

    /*
     * isr_syscall uses PUSHA before entering C.
     *
     * At this point the saved register frame is:
     *
     *   EDI
     *   ESI
     *   EBP
     *   ESP
     *   EBX
     *   EDX
     *   ECX
     *   EAX
     *
     * syscall number is therefore the final PUSHA value.
     *
     * The current ISR does not pass the frame explicitly,
     * so the first milestone only verifies that the syscall
     * gate itself works.
     */
    __asm__ volatile (
        "mov %%eax, %0"
        : "=r"(eax)
    );

    /*
     * The user program intentionally executes:
     *
     *     mov eax, 1
     *     int 0x80
     *
     * The exact EAX value is not yet part of the public
     * syscall ABI; this milestone verifies Ring-3 entry
     * and return through the DPL-3 IDT gate.
     */
    (void)eax;

    console_info("Ring 3 system call received: PASS");
}
