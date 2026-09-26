#include "idt.h"

#define IDT_ENTRIES 256

#define KERNEL_CODE_SEGMENT 0x08
#define IDT_INTERRUPT_GATE 0x8E
#define SYSCALL_VECTOR 128
#define SYSCALL_GATE 0xEE

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void idt_load(struct idt_ptr *idtp);

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

extern void isr_default(void);
extern void isr_syscall(void);

static void idt_set_gate(
    int number,
    uint32_t base,
    uint16_t selector,
    uint8_t flags
)
{
    idt[number].base_low = base & 0xFFFF;
    idt[number].selector = selector;
    idt[number].always0 = 0;
    idt[number].flags = flags;
    idt[number].base_high = (base >> 16) & 0xFFFF;
}

void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(
            i,
            (uint32_t)isr_default,
            KERNEL_CODE_SEGMENT,
            IDT_INTERRUPT_GATE
        );
    }

    void (*exceptions[32])(void) = {
        isr0, isr1, isr2, isr3,
        isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11,
        isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19,
        isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27,
        isr28, isr29, isr30, isr31
    };

    for (int i = 0; i < 32; i++) {
        idt_set_gate(
            i,
            (uint32_t)exceptions[i],
            KERNEL_CODE_SEGMENT,
            IDT_INTERRUPT_GATE
        );
    }

    void (*irqs[16])(void) = {
        irq0, irq1, irq2, irq3,
        irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11,
        irq12, irq13, irq14, irq15
    };

    for (int i = 0; i < 16; i++) {
        idt_set_gate(
            32 + i,
            (uint32_t)irqs[i],
            KERNEL_CODE_SEGMENT,
            IDT_INTERRUPT_GATE
        );
    }

    /* Ring-3 callable system call gate. */
    idt_set_gate(
        SYSCALL_VECTOR,
        (uint32_t)isr_syscall,
        KERNEL_CODE_SEGMENT,
        SYSCALL_GATE
    );

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    idt_load(&idtp);
}

static int idt_gate_valid(int number)
{
    uint32_t base =
        ((uint32_t)idt[number].base_high << 16) |
        idt[number].base_low;

    if (base == 0)
        return 0;

    if (idt[number].selector != KERNEL_CODE_SEGMENT)
        return 0;

    if ((idt[number].flags & 0x80) == 0)
        return 0;

    return 1;
}

int idt_run_self_test(void)
{
    if (idtp.limit != sizeof(idt) - 1)
        return 0;

    if (idtp.base != (uint32_t)&idt)
        return 0;

    for (int i = 0; i < 32; i++) {
        if (!idt_gate_valid(i))
            return 0;
    }

    for (int i = 0; i < 16; i++) {
        if (!idt_gate_valid(32 + i))
            return 0;
    }

    if (!idt_gate_valid(SYSCALL_VECTOR))
        return 0;

    if ((idt[SYSCALL_VECTOR].flags & 0x60) != 0x60)
        return 0;

    return 1;
}
