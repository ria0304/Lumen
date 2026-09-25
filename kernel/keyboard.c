#include "idt.h"
#include "pic.h"

#define KBD_DATA_PORT 0x60
#define KBD_PIC1_DATA 0x21

extern void terminal_putchar(char c);

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static const char kbd_us_map[128] = {
    0,  0x1B, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm',
    ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0,       /* ... and some more */
    0,  0x7F, 0, 0, 0, 0, 0, 0, 0, 0,
    0x37, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   /* ... */
};

void kbd_init(void)
{
    /* Enable keyboard IRQ1 (unmask bit 1 on the master PIC) */
    uint8_t tmp = inb(KBD_PIC1_DATA);
    outb(KBD_PIC1_DATA, tmp & ~0x02);
}

void kbd_handler(void)
{
    uint8_t scancode = inb(KBD_DATA_PORT);

    /* Send EOI to PIC */
    pic_send_eoi(1);

    /* Handle scancode */
    if (scancode < 128) {
        char c = kbd_us_map[scancode];
        if (c != 0) {
            terminal_putchar(c);
        }
    }
}
