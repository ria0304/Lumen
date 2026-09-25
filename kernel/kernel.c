#include <stdint.h>
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "heap.h"
#include "console.h"

extern void kbd_init(void);

#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

static volatile uint32_t timer_ticks = 0;

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

    console_info("Lumer kernel online!");
    console_info("VGA text driver: OK");
    console_info("Protected mode: 32-bit");

    idt_init();
    console_info("IDT initialized: OK");

    pic_init();
    console_info("PIC initialized: OK");

    pit_init(100);
    console_info("PIT initialized: 100 Hz");

    kbd_init();
    console_info("Keyboard initialized: OK");

    heap_init();
    console_info("Memory allocator: OK");

    char *buffer = (char *)kmalloc(64);

    if (buffer != 0) {
        const char message[] = "Dynamic buffer allocation: WORKING";

        int i = 0;
        while (message[i] != '\0') {
            buffer[i] = message[i];
            i++;
        }
        buffer[i] = '\0';

        console_info(buffer);
        console_info("Heap allocation test passed");

        uint32_t used = heap_used();
        char digits[10];
        int count = 0;

        while (used > 0) {
            digits[count++] = '0' + (used % 10);
            used /= 10;
        }

        if (count == 0)
            terminal_putchar('0');

        while (count > 0)
            terminal_putchar(digits[--count]);

        terminal_write(" bytes\n");
    } else {
        console_error("Memory allocation: FAILED");
    }

    console_info("Enabling timer + keyboard interrupts...");

    __asm__ volatile ("sti");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
