#include <stdint.h>
#include "serial.h"

#define COM1            0x3F8
#define COM_DATA        (COM1 + 0)
#define COM_INT_ENABLE  (COM1 + 1)
#define COM_DIVISOR_LO  (COM1 + 0)
#define COM_DIVISOR_HI  (COM1 + 1)
#define COM_FIFO_CTRL   (COM1 + 2)
#define COM_LINE_CTRL   (COM1 + 3)
#define COM_MODEM_CTRL  (COM1 + 4)
#define COM_LINE_STATUS (COM1 + 5)

/* Bit 5 of the line status register: transmitter holding register
 * empty, i.e. safe to write another byte. */
#define LSR_TX_EMPTY 0x20

/* QEMU's -device isa-debug-exit default port. */
#define DEBUG_EXIT_PORT 0x501

static int serial_ready = 0;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void)
{
    serial_ready = 0;

    /* 38400 baud is what QEMU's default 16550 emulation expects
     * for a divisor of 3 (115200 / 3). The exact rate is
     * irrelevant here -- output is never read by a real terminal,
     * only by 'make test' -- but the divisors still have to be
     * programmed so the UART is in 8N1 mode. */
    outb(COM_INT_ENABLE, 0x00);
    outb(COM_DIVISOR_LO, 0x03);
    outb(COM_DIVISOR_HI, 0x00);

    /* 8 bits, no parity, one stop bit. */
    outb(COM_LINE_CTRL, 0x03);

    /* Enable and clear the FIFOs, 14-byte trigger threshold. */
    outb(COM_FIFO_CTRL, 0xC7);

    /* DTR | RTS | OUT2. OUT2 matters on real hardware: without it
     * the UART interrupt line stays deasserted. */
    outb(COM_MODEM_CTRL, 0x0B);

    serial_ready = 1;
}

int serial_is_ready(void)
{
    return serial_ready;
}

void serial_putchar(char c)
{
    if (!serial_ready)
        return;

    /*
     * Bounded spin on the transmitter. A real UART drains in a few
     * microseconds; this is only reachable if the line status never
     * clears, in which case dropping the character beats hanging the
     * whole kernel.
     */
    for (uint32_t spins = 0; spins < 100000U; spins++) {

        if (inb(COM_LINE_STATUS) & LSR_TX_EMPTY)
            break;
    }

    outb(COM_DATA, (uint8_t)c);
}

void serial_write(const char *message)
{
    if (!serial_ready)
        return;

    for (int i = 0; message[i] != '\0'; i++)
        serial_putchar(message[i]);
}

void serial_print_uint(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        serial_putchar('0');
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (count > 0)
        serial_putchar(digits[--count]);
}

void qemu_exit(int code)
{
    /*
     * Only meaningful under 'make test', which passes
     * -device isa-debug-exit. On real hardware this writes to an
     * unclaimed port and is ignored.
     */
    outw(DEBUG_EXIT_PORT, (uint16_t)(((code & 0xFF) << 1) | 1));

    for (;;)
        __asm__ volatile ("hlt");
}
