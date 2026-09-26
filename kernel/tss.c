#include "tss.h"
#include "privilege.h"

tss_t kernel_tss;

static uint16_t read_task_register(void)
{
    uint16_t value;

    __asm__ volatile (
        "str %0"
        : "=r"(value)
    );

    return value;
}

void tss_init(void)
{
    kernel_tss.prev_tss = 0;
    kernel_tss.esp0 = 0x90000;
    kernel_tss.ss0 = KERNEL_DATA_SELECTOR;

    kernel_tss.esp1 = 0;
    kernel_tss.ss1 = 0;
    kernel_tss.esp2 = 0;
    kernel_tss.ss2 = 0;

    kernel_tss.cr3 = 0;
    kernel_tss.eip = 0;
    kernel_tss.eflags = 0;
    kernel_tss.eax = 0;
    kernel_tss.ecx = 0;
    kernel_tss.edx = 0;
    kernel_tss.ebx = 0;
    kernel_tss.esp = 0;
    kernel_tss.ebp = 0;
    kernel_tss.esi = 0;
    kernel_tss.edi = 0;

    kernel_tss.es = KERNEL_DATA_SELECTOR;
    kernel_tss.cs = KERNEL_CODE_SELECTOR;
    kernel_tss.ss = KERNEL_DATA_SELECTOR;
    kernel_tss.ds = KERNEL_DATA_SELECTOR;
    kernel_tss.fs = KERNEL_DATA_SELECTOR;
    kernel_tss.gs = KERNEL_DATA_SELECTOR;

    kernel_tss.ldt = 0;
    kernel_tss.trap = 0;
    kernel_tss.iomap_base = sizeof(tss_t);
}

void tss_set_esp0(uint32_t esp0)
{
    kernel_tss.esp0 = esp0;
}

int tss_run_self_test(void)
{
    if (kernel_tss.esp0 == 0)
        return 0;

    if (kernel_tss.ss0 != KERNEL_DATA_SELECTOR)
        return 0;

    if (kernel_tss.cs != KERNEL_CODE_SELECTOR)
        return 0;

    if (kernel_tss.ss != KERNEL_DATA_SELECTOR)
        return 0;

    if (kernel_tss.ds != KERNEL_DATA_SELECTOR)
        return 0;

    if (kernel_tss.iomap_base != sizeof(tss_t))
        return 0;

    if (read_task_register() != 0x28)
        return 0;

    return 1;
}
