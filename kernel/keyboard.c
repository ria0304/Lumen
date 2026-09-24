#include "idt.h"
#include "pic.h"

#define KBD_DATA_PORT 0x60

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
    /* Enable keyboard IRQ1 */
    uint8_t tmp = inb(0x21);
    outb(0x21, tmp & ~0x02);
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