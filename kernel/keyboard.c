#include <stdint.h>
#include "idt.h"
#include "pic.h"
#include "console.h"
#include "line_editor.h"
#include "keycodes.h"

#define KBD_DATA_PORT 0x60
#define KBD_PIC1_DATA 0x21

/* Bit 7 of a scancode marks a key release rather than a press. */
#define SCANCODE_RELEASE 0x80
#define SCANCODE_MASK    0x7F

/* Extended keys arrive as a 0xE0 prefix followed by a second
 * scancode. */
#define SCANCODE_EXTENDED_PREFIX 0xE0

/* Modifiers that carry no character of their own. */
#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36
#define SCANCODE_LCTRL  0x1D
#define SCANCODE_RCTRL  0x9D
#define SCANCODE_CAPSLOCK 0x3A

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
    'q',  'w',  'e',  'r', 't',  'y', 'u', 'i',
    'o',  'p',  '[', ']', '\n', 0,   'a', 's',
    'd',  'f',  'g',  'h', 'j',  'k', 'l', ';',
    '\'', '`',  0,   '\\', 'z',  'x', 'c', 'v',
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
    'Q',  'W',  'E',  'R',  'T',  'Y', 'U', 'I',
    'O',  'P',  '{', '}', '\n', 0,   'A', 'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L', ':',
    '"',  '~',  0,   '|',  'Z',  'X',  'C', 'V',
    'B',  'N',  'M',  '<',  '>',  '?', 0,   '*',
    0,    ' ',  0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0,
    0,    0,    0,   0,    0,   0,   0,   0
};

/* The 0xE0-prefixed scancodes that carry a control key rather than a
 * character. Anything not listed here is consumed and dropped. */
static uint8_t extended_to_keycode(uint8_t scancode)
{
    switch (scancode) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PAGE_UP;
        case 0x51: return KEY_PAGE_DOWN;
        case 0x53: return KEY_DELETE;
        default:   return KEY_NONE;
    }
}

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock_on = 0;
static int extended_pending = 0;

void kbd_init(void)
{
    /* Enable keyboard IRQ1 on the master PIC. */
    uint8_t tmp = inb(KBD_PIC1_DATA);
    outb(KBD_PIC1_DATA, tmp & ~0x02);
}

/*
 * Caps Lock inverts Shift for letters only, which is why the two
 * tables are indexed by scancode and the letter case is fixed up
 * afterwards rather than Caps being folded into a third table.
 */
static char apply_caps(char c)
{
    if (!caps_lock_on)
        return c;

    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');

    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');

    return c;
}

void kbd_handler(void)
{
    uint8_t scancode = inb(KBD_DATA_PORT);

    /*
     * A 0xE0 prefix means the next byte belongs to the extended key
     * set. Consume the prefix itself and leave the flag set; the
     * prefix carries no character.
     */
    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        extended_pending = 1;
        pic_end_of_interrupt(1);
        return;
    }

    if (extended_pending) {
        extended_pending = 0;
        uint8_t code = extended_to_keycode(scancode & SCANCODE_MASK);

        if (code != KEY_NONE)
            line_editor_handle_char(code);

        pic_end_of_interrupt(1);
        return;
    }

    if (scancode & SCANCODE_RELEASE) {
        uint8_t key = scancode & SCANCODE_MASK;

        if (key == SCANCODE_LSHIFT || key == SCANCODE_RSHIFT)
            shift_pressed = 0;

        if (key == SCANCODE_LCTRL || key == SCANCODE_RCTRL)
            ctrl_pressed = 0;

        pic_end_of_interrupt(1);
        return;
    }

    if (scancode == SCANCODE_LSHIFT || scancode == SCANCODE_RSHIFT) {
        shift_pressed = 1;
        pic_end_of_interrupt(1);
        return;
    }

    if (scancode == SCANCODE_LCTRL) {
        ctrl_pressed = 1;
        pic_end_of_interrupt(1);
        return;
    }

    if (scancode == SCANCODE_CAPSLOCK) {
        caps_lock_on = !caps_lock_on;
        pic_end_of_interrupt(1);
        return;
    }

    /*
     * Ctrl combinations. Only the four the shell acts on are
     * recognised; the rest of the Ctrl space is dropped so it cannot
     * be mistaken for a control character typed directly.
     */
    if (ctrl_pressed) {
        uint8_t key = 0;

        if (scancode == 0x2E) key = KEY_CTRL_C;
        else if (scancode == 0x20) key = KEY_CTRL_D;
        else if (scancode == 0x26) key = KEY_CTRL_L;
        else if (scancode == 0x18) key = KEY_CTRL_U;

        if (key != KEY_NONE)
            line_editor_handle_char(key);

        pic_end_of_interrupt(1);
        return;
    }

    if (scancode < 128) {
        char c = shift_pressed
                     ? kbd_us_shift_map[scancode]
                     : kbd_us_map[scancode];

        if (c != 0)
            line_editor_handle_char((uint8_t)apply_caps(c));
    }

    pic_end_of_interrupt(1);
}
