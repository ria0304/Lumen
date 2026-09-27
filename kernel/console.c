#include <stdint.h>
#include "console.h"
#include "serial.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

static int terminal_row = 0;
static int terminal_column = 0;

static volatile uint16_t *vga =
    (volatile uint16_t *)VGA_MEMORY;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static void terminal_update_cursor(void)
{
    uint16_t position =
        (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);

    outb(0x3D4, 0x0F);
    outb(0x3D5, position & 0xFF);

    outb(0x3D4, 0x0E);
    outb(0x3D5, (position >> 8) & 0xFF);
}

static void terminal_scroll(void)
{
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga[(y - 1) * VGA_WIDTH + x] =
                vga[y * VGA_WIDTH + x];
        }
    }

    for (int x = 0; x < VGA_WIDTH; x++) {
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            ((uint16_t)VGA_COLOR << 8) | ' ';
    }

    terminal_row = VGA_HEIGHT - 1;
    terminal_column = 0;
    terminal_update_cursor();
}

static void terminal_newline(void)
{
    terminal_column = 0;
    terminal_row++;

    if (terminal_row >= VGA_HEIGHT)
        terminal_scroll();
    else
        terminal_update_cursor();
}

void terminal_clear(void)
{
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int index = y * VGA_WIDTH + x;
            vga[index] = ((uint16_t)VGA_COLOR << 8) | ' ';
        }
    }

    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
}

void terminal_putchar(char c)
{
    serial_putchar(c);

    if (c == '\n') {
        terminal_newline();
        return;
    }

    if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;

            int index =
                terminal_row * VGA_WIDTH + terminal_column;

            vga[index] =
                ((uint16_t)VGA_COLOR << 8) | ' ';
            terminal_update_cursor();
        }

        return;
    }

    int index =
        terminal_row * VGA_WIDTH + terminal_column;

    vga[index] =
        ((uint16_t)VGA_COLOR << 8) | (uint8_t)c;

    terminal_column++;

    if (terminal_column >= VGA_WIDTH)
        terminal_newline();
    else
        terminal_update_cursor();
}

void terminal_write(const char *message)
{
    for (int i = 0; message[i] != '\0'; i++)
        terminal_putchar(message[i]);
}

void terminal_write_u32(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        terminal_putchar('0');
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (count > 0)
        terminal_putchar(digits[--count]);
}

void terminal_write_hex8(uint8_t value)
{
    const char hex[] = "0123456789ABCDEF";

    terminal_putchar(hex[(value >> 4) & 0xF]);
    terminal_putchar(hex[value & 0xF]);
}

void console_info(const char *message)
{
    terminal_write("[INFO] ");
    terminal_write(message);
    terminal_putchar('\n');
}

void console_warn(const char *message)
{
    terminal_write("[WARN] ");
    terminal_write(message);
    terminal_putchar('\n');
}

void console_error(const char *message)
{
    terminal_write("[ERROR] ");
    terminal_write(message);
    terminal_putchar('\n');
}
