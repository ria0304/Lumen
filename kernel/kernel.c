#include <stdint.h>
#include "gdt.h"
#include "tss.h"
#include "scheduler.h"
#include "paging.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "heap.h"
#include "console.h"
#include "line_editor.h"
#include "shell.h"
#include "task.h"

extern void kbd_init(void);

#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

volatile uint32_t timer_ticks = 0;

void exception_handler(uint32_t vector)
{
    console_error("CPU EXCEPTION RECEIVED");

    switch (vector) {
        case 0:
            console_error("Exception 0: Divide by zero");
            break;

        case 1:
            console_error("Exception 1: Debug");
            break;

        case 2:
            console_error("Exception 2: Non-maskable interrupt");
            break;

        case 3:
            console_error("Exception 3: Breakpoint");
            break;

        case 4:
            console_error("Exception 4: Overflow");
            break;

        case 5:
            console_error("Exception 5: Bound range exceeded");
            break;

        case 6:
            console_error("Exception 6: Invalid opcode");
            break;

        case 7:
            console_error("Exception 7: Device not available");
            break;

        case 8:
            console_error("Exception 8: Double fault");
            break;

        case 13:
            console_error("Exception 13: General protection fault");
            break;

        case 14:
            console_error("Exception 14: Page fault");
            break;

        default:
            console_error("Exception: Unhandled CPU exception");
            break;
    }

    console_error("System halted");

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

    tss_init();
    gdt_init();
    tss_load();
    scheduler_init();

    console_info("Lumer kernel online!");
    console_info("VGA text driver: OK");
    console_info("Protected mode: 32-bit");

    idt_init();
    paging_init();
    console_info("IDT initialized: OK");

    pic_init();
    console_info("PIC initialized: OK");

    pit_init(100);
    console_info("PIT initialized: 100 Hz");

    kbd_init();
    console_info("Keyboard initialized: OK");

    line_editor_init();
    console_info("Line editor initialized: OK");

    shell_init();

    task_init();

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
