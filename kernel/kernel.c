#include <stdint.h>
#include "gdt.h"
#include "tss.h"
#include "scheduler.h"
#include "paging.h"
#include "frame.h"
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

static void print_hex32(uint32_t value)
{
    const char hex[] = "0123456789ABCDEF";

    terminal_write("0x");

    for (int i = 7; i >= 0; i--) {
        uint8_t digit =
            (value >> (i * 4)) & 0xF;

        terminal_putchar(hex[digit]);
    }
}

void exception_handler(
    uint32_t vector,
    uint32_t error_code
)
{
    console_error("CPU EXCEPTION RECEIVED");

    terminal_write("[ERROR] Vector: ");
    print_hex32(vector);
    terminal_putchar('\n');

    terminal_write("[ERROR] Error code: ");
    print_hex32(error_code);
    terminal_putchar('\n');

    if (vector == 14) {
        uint32_t cr2;

        __asm__ volatile (
            "mov %%cr2, %0"
            : "=r"(cr2)
        );

        terminal_write("[ERROR] Fault address: ");
        print_hex32(cr2);
        terminal_putchar('\n');
    }

    switch (vector) {
        case 0:  console_error("Exception 0: Divide by zero"); break;
        case 1:  console_error("Exception 1: Debug"); break;
        case 2:  console_error("Exception 2: Non-maskable interrupt"); break;
        case 3:  console_error("Exception 3: Breakpoint"); break;
        case 4:  console_error("Exception 4: Overflow"); break;
        case 5:  console_error("Exception 5: Bound range exceeded"); break;
        case 6:  console_error("Exception 6: Invalid opcode"); break;
        case 7:  console_error("Exception 7: Device not available"); break;
        case 8:  console_error("Exception 8: Double fault"); break;
        case 9:  console_error("Exception 9: Coprocessor segment overrun"); break;
        case 10: console_error("Exception 10: Invalid TSS"); break;
        case 11: console_error("Exception 11: Segment not present"); break;
        case 12: console_error("Exception 12: Stack-segment fault"); break;
        case 13: console_error("Exception 13: General protection fault"); break;
        case 14: console_error("Exception 14: Page fault"); break;
        case 15: console_error("Exception 15: Reserved"); break;
        case 16: console_error("Exception 16: x87 floating-point"); break;
        case 17: console_error("Exception 17: Alignment check"); break;
        case 18: console_error("Exception 18: Machine check"); break;
        case 19: console_error("Exception 19: SIMD floating-point"); break;
        case 20: console_error("Exception 20: Virtualization"); break;
        case 21: console_error("Exception 21: Control protection"); break;
        default: console_error("Exception: Reserved/unknown CPU exception"); break;
    }

    console_error("System halted");

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void irq_unhandled_handler(uint32_t irq)
{
    terminal_write("[WARN] Unhandled hardware IRQ: ");
    print_hex32(irq);
    terminal_putchar('\n');

    pic_send_eoi((uint8_t)irq);
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

    console_info("Lumer kernel online!");
    console_info("VGA text driver: OK");
    console_info("Protected mode: 32-bit");

    if (gdt_run_self_test()) {
        console_info("GDT self-test: PASS");
    } else {
        console_error("GDT self-test: FAILED");
    }

    if (tss_run_self_test()) {
        console_info("TSS self-test: PASS");
    } else {
        console_error("TSS self-test: FAILED");
    }

    idt_init();

    if (idt_run_self_test()) {
        console_info("IDT self-test: PASS");
    } else {
        console_error("IDT self-test: FAILED");
    }

    paging_init();
    frame_init();

    if (frame_run_self_test()) {
        console_info("Frame allocator test: PASS");
    } else {
        console_info("Frame allocator test: FAILED");
    }
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

    if (task_run_self_test()) {
        console_info("Task manager self-test: PASS");
    } else {
        console_error("Task manager self-test: FAILED");
    }

    scheduler_init();

    if (scheduler_run_self_test()) {
        console_info("Scheduler self-test: PASS");
    } else {
        console_error("Scheduler self-test: FAILED");
    }

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
