#include "idt.h"

#define IDT_ENTRIES 256
#define KERNEL_CODE_SEGMENT 0x08
#define IDT_INTERRUPT_GATE 0x8E

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void idt_load(struct idt_ptr *idtp);
extern void isr0(void);
extern void isr3(void);
extern void irq0(void);

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
        idt_set_gate(i, 0, 0, 0);
    }

    /* CPU exceptions */
    idt_set_gate(
        0,
        (uint32_t)isr0,
        KERNEL_CODE_SEGMENT,
        IDT_INTERRUPT_GATE
    );

    idt_set_gate(
        3,
        (uint32_t)isr3,
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

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    idt_load(&idtp);
}
