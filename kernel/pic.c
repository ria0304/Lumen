#include "pic.h"

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

void pic_init(void)
{
    /* Start initialization sequence */
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);

    /* Remap IRQs:
       Master PIC: IRQ 0-7  -> vectors 32-39
       Slave PIC:  IRQ 8-15 -> vectors 40-47
    */
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);

    /* Tell master about slave at IRQ2 */
    outb(PIC1_DATA, 0x04);

    /* Tell slave its cascade identity */
    outb(PIC2_DATA, 0x02);

    /* 8086/88 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /*
     * Mask everything by default. Only vectors that have a real
     * IDT handler installed should ever be unmasked -- an unmasked
     * IRQ with no handler leads straight to a triple fault.
     * IRQ0 (timer) and IRQ1 (keyboard) are enabled here; the slave
     * PIC's cascade line (IRQ2) is left masked since nothing behind
     * it is handled yet.
     */
    outb(PIC1_DATA, 0xFC); /* mask all master IRQs except 0 and 1 */
    outb(PIC2_DATA, 0xFF); /* mask all slave IRQs */
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}
