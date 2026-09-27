#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

/*
 * Minimal interrupt-disabling critical sections.
 *
 * Lumen is single-processor, so the only thing that can interrupt a
 * kernel thread mid-update is an interrupt: the PIT preempts through
 * scheduler_irq() on every tick, and any task that follows can call
 * back into a shared data structure. Disabling interrupts is therefore
 * a complete mutual-exclusion primitive here -- there is no second CPU
 * that could observe the half-updated state even with interrupts on.
 *
 * The flag is saved and restored rather than blindly re-enabled, so
 * wrapping a region that was already running with interrupts off does
 * not silently re-enable them for the rest of the caller.
 */

typedef uint32_t irqflags_t;

#define IRQFLAG_INTERRUPTS 0x200U

static inline irqflags_t irq_save_disable(void)
{
    irqflags_t flags;

    __asm__ volatile (
        "pushfl\n"
        "popl %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static inline void irq_restore(irqflags_t flags)
{
    if (flags & IRQFLAG_INTERRUPTS) {
        __asm__ volatile ("sti");
    }
}

#endif
