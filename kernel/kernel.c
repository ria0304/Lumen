#include <stdint.h>
#include "idt.h"
#include "pic.h"
#include "pit.h"

extern void kbd_init(void);

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

static int terminal_row = 0;
static int terminal_column = 0;
static volatile uint32_t timer_ticks = 0;

static volatile unsigned short *vga =
    (volatile unsigned short *)VGA_MEMORY;

static void terminal_clear(void)
{
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int index = y * VGA_WIDTH + x;
            vga[index] = ((unsigned short)VGA_COLOR << 8) | ' ';
        }
    }

    terminal_row = 0;
    terminal_column = 0;
}

void terminal_putchar(char c)
{
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= VGA_HEIGHT)
            terminal_row = 0;

        return;
    }

    int index = terminal_row * VGA_WIDTH + terminal_column;

    vga[index] = ((unsigned short)VGA_COLOR << 8) | c;

    terminal_column++;

    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= VGA_HEIGHT)
            terminal_row = 0;
    }
}

static void terminal_write(const char *message)
{
    for (int i = 0; message[i] != '\0'; i++) {
        terminal_putchar(message[i]);
    }
}

void exception_handler(uint32_t vector)
{
    terminal_write("\nEXCEPTION RECEIVED\n");

    if (vector == 0) {
        terminal_write("Exception: Divide by zero\n");
    } else if (vector == 3) {
        terminal_write("Exception: Breakpoint\n");
    } else {
        terminal_write("Exception: Unknown\n");
    }

    terminal_write("IDT: WORKING\n");

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void timer_handler(void)
{
    timer_ticks++;

    /*
     * Display the low 16 bits of the timer tick count
     * in hexadecimal at the top-right of the screen.
     */

    if (timer_ticks % 100 == 0) {
        volatile unsigned short *seconds_display =
            (volatile unsigned short *)VGA_MEMORY;

        const char text[] = "SEC:";
        for (int i = 0; i < 4; i++) {
            seconds_display[80 + 70 + i] =
                ((unsigned short)VGA_COLOR << 8) | text[i];
        }

        uint32_t seconds = timer_ticks / 100;
        const char hex[] = "0123456789ABCDEF";

        for (int i = 0; i < 4; i++) {
            uint8_t digit = seconds & 0xF;
            seconds_display[80 + 78 - i] =
                ((unsigned short)VGA_COLOR << 8) | hex[digit];
            seconds >>= 4;
        }
    }
    volatile unsigned short *timer_display =
        (volatile unsigned short *)VGA_MEMORY;

    const char hex[] = "0123456789ABCDEF";
    uint32_t value = timer_ticks;

    for (int i = 0; i < 8; i++) {
        uint8_t digit = value & 0xF;
        timer_display[79 - i] =
            ((unsigned short)VGA_COLOR << 8) | hex[digit];
        value >>= 4;
    }

    /*
     * Send End Of Interrupt to the master PIC.
     */
    pic_send_eoi(0);
}

void kmain(void)
{
    terminal_clear();

    terminal_write("Lumer kernel online!\n");
    terminal_write("VGA text driver: OK\n");
    terminal_write("Protected mode: 32-bit\n");

    idt_init();
    terminal_write("IDT initialized: OK\n");

    pic_init();
    terminal_write("PIC initialized: OK\n");

    pit_init(100);
    terminal_write("PIT initialized: 100 Hz\n");

    kbd_init();
    terminal_write("Keyboard initialized: OK\n");

    terminal_write("Enabling timer + keyboard interrupts...\n");

    __asm__ volatile ("sti");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
