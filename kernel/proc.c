#include "proc.h"
#include "console.h"
#include "task.h"
#include "heap.h"
#include "frame.h"
extern volatile uint32_t timer_ticks;
void proc_uname(void) {
    terminal_write("Lumen lumenv1 0.3 i386 LumenFS preempt-rr\n");
}
void proc_top(void) {
    terminal_write("PID PPID STATE\n");
    for (uint32_t i = 0; i < 16; i++) {
        const task_t *t = task_get(i);
        if (!t || t->state == TASK_UNUSED) continue;
        terminal_write_u32(t->id); terminal_write(" ");
        terminal_write_u32(t->parent_id); terminal_write(" ");
        terminal_write_u32((uint32_t)t->state); terminal_putchar('\n');
    }
    terminal_write("ticks="); terminal_write_u32(timer_ticks); terminal_putchar('\n');
    terminal_write("heap_used="); terminal_write_u32(heap_used()); terminal_putchar('\n');
}
int proc_run_self_test(void) {
    return task_get(0) != 0;
}
