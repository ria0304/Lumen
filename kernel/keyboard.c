#include <stdint.h>
#include "idt.h"
#include "pic.h"
#include "console.h"
#include "line_editor.h"

#define KBD_DATA_PORT 0x60
#define KBD_PIC1_DATA 0x21

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

/* US keyboard, unshifted */
static const char kbd_us_map[128] = {
    0,    27,   '1', '2', '3', '4', '5', '6',
    '7',  '8',  '9', '0', '-', '=', '\b', '\t',
    'q',  'w',  'e', 'r', 't', 'y', 'u', 'i',
    'o',  'p',  '[', ']', '\n', 0,   'a', 's',
    'd',  'f',  'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v',
    'b',  'n',  'm', ',', '.', '/', 0,   '*',
    0,    ' ',  0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0
};

/* US keyboard, shifted */
static const char kbd_us_shift_map[128] = {
    0,    27,   '!', '@', '#', '$', '%', '^',
    '&',  '*',  '(', ')', '_', '+', '\b', '\t',
    'Q',  'W',  'E', 'R', 'T', 'Y', 'U', 'I',
    'O',  'P',  '{', '}', '\n', 0,   'A', 'S',
    'D',  'F',  'G', 'H', 'J', 'K', 'L', ':',
    '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V',
    'B',  'N',  'M', '<', '>', '?', 0,   '*',
    0,    ' ',  0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0
};

static int shift_pressed = 0;

void kbd_init(void)
{
    /* Enable keyboard IRQ1 on the master PIC. */
    uint8_t tmp = inb(KBD_PIC1_DATA);
    outb(KBD_PIC1_DATA, tmp & ~0x02);
}

void kbd_handler(void)
{
    uint8_t scancode = inb(KBD_DATA_PORT);

    /* Send EOI to PIC. */
    pic_send_eoi(1);

    /*
     * Bit 7 set means this is a key-release scancode.
     * We only need release events for tracking Shift.
     */
    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;

        if (key == 0x2A || key == 0x36) {
            shift_pressed = 0;
        }

        return;
    }

    /* Left Shift = 0x2A, Right Shift = 0x36 */
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }

    if (scancode < 128) {
        char c;

        if (shift_pressed)
            c = kbd_us_shift_map[scancode];
        else
            c = kbd_us_map[scancode];

        if (c != 0)
            line_editor_handle_char(c);
    }
}
