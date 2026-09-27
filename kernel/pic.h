#ifndef PIC_H
#define PIC_H

#include <stdint.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21

#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

/* OCW3 read-ISR-select, used to tell a real IRQ7/IRQ15 from a
 * spurious one. See pic_is_spurious(). */
#define PIC_READ_ISR 0x0B

void pic_init(void);

/*
 * Returns 1 if 'irq' is a spurious interrupt, i.e. the PIC raised
 * IRQ7 or IRQ15 but its in-service register says nothing is actually
 * being serviced. Acknowledging a spurious interrupt with a real EOI
 * would unbalance the PIC's in-service bookkeeping and can cause a
 * genuine interrupt to be masked out later.
 *
 * Only the cascade lines (IRQ7 on the master, IRQ15 on the slave) can
 * be spurious; every other IRQ returns 0.
 */
int pic_is_spurious(uint8_t irq);

/* Send EOI, unless the interrupt turned out to be spurious. This is
 * the intended single exit point for an IRQ handler. */
void pic_end_of_interrupt(uint8_t irq);

#endif
