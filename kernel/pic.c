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

int pic_is_spurious(uint8_t irq)
{
    /*
     * IRQ7 on the master and IRQ15 on the slave share their vector
     * line with the cascade, and a glitch on that line looks exactly
     * like an interrupt. The way to tell them apart is to ask the
     * PIC what it thinks it is servicing: if the corresponding
     * in-service bit is clear, nothing is really in progress and the
     * interrupt was spurious.
     *
     * Sending a real EOI for a spurious interrupt would clear an
     * in-service bit that was never set, so a later genuine
     * interrupt on that line can be wrongly masked.
     */
    if (irq == 7) {
        outb(PIC1_COMMAND, PIC_READ_ISR);

        if ((inb(PIC1_COMMAND) & 0x80) == 0)
            return 1;
    }

    if (irq == 15) {
        outb(PIC2_COMMAND, PIC_READ_ISR);

        if ((inb(PIC2_COMMAND) & 0x80) == 0)
            return 1;
    }

    return 0;
}

void pic_end_of_interrupt(uint8_t irq)
{
    if (pic_is_spurious(irq))
        return;

    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}
