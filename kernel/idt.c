#include "idt.h"

#define IDT_ENTRIES 256
#define KERNEL_CODE_SEGMENT 0x08
#define IDT_INTERRUPT_GATE 0x8E

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
extern void isr13(void);
extern void isr14(void);
extern void isr_default(void);
extern void irq0(void);
extern void irq1(void);

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
    /*
     * Install a safe default handler for unused hardware/software
     * interrupt vectors. CPU exception vectors are installed below
     * with their correct ISR stubs.
     */
    for (int i = 32; i < IDT_ENTRIES; i++) {
        idt_set_gate(
            i,
            (uint32_t)isr_default,
            KERNEL_CODE_SEGMENT,
            IDT_INTERRUPT_GATE
        );
    }

    /* CPU exceptions */
    idt_set_gate(
        0,
        (uint32_t)isr0,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        1,
        (uint32_t)isr1,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        2,
        (uint32_t)isr2,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        3,
        (uint32_t)isr3,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        4,
        (uint32_t)isr4,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        5,
        (uint32_t)isr5,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        6,
        (uint32_t)isr6,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        7,
        (uint32_t)isr7,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        8,
        (uint32_t)isr8,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        13,
        (uint32_t)isr13,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        14,
        (uint32_t)isr14,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    /* Hardware timer IRQ0 -> vector 32 */
    idt_set_gate(
        32,
        (uint32_t)irq0,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    /* Keyboard IRQ1 -> vector 33 */
    idt_set_gate(
        33,
        (uint32_t)irq1,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    idt_load(&idtp);
}
