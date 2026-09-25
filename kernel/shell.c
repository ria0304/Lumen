#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "heap.h"

extern volatile uint32_t timer_ticks;

static void shell_prompt(void)
{
    terminal_write("Lumer> ");
}

static void shell_print_uint(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0) {
        digits[count++] = '0' + (value % 10);
        value /= 10;
    }

    while (count > 0)
        terminal_putchar(digits[--count]);
}

static int string_starts_with(const char *text, const char *prefix)
{
    int i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i])
            return 0;

        i++;
    }

    return 1;
}

void shell_init(void)
{
    shell_prompt();
}

void shell_handle_line(const char *line)
{
    if (line[0] == '\0') {
        shell_prompt();
        return;
    }

    if (line[0] == 'h' &&
        line[1] == 'e' &&
        line[2] == 'l' &&
        line[3] == 'p' &&
        line[4] == '\0') {

        console_info("Commands: help, clear, echo, about, version, mem, uptime");
    }
    else if (string_starts_with(line, "echo ")) {
        terminal_write(line + 5);
        terminal_putchar('\n');
    }
    else if (line[0] == 'c' &&
             line[1] == 'l' &&
             line[2] == 'e' &&
             line[3] == 'a' &&
             line[4] == 'r' &&
             line[5] == '\0') {
        terminal_clear();
    }
    else if (line[0] == 'a' &&
             line[1] == 'b' &&
             line[2] == 'o' &&
             line[3] == 'u' &&
             line[4] == 't' &&
             line[5] == '\0') {
        console_info("Lumer OS.");
        console_info("Built from scratch in C and x86 assembly.");
        console_info("Custom bootloader, kernel, interrupts, memory, keyboard, and shell.");
    }
    else if (line[0] == 'v' &&
             line[1] == 'e' &&
             line[2] == 'r' &&
             line[3] == 's' &&
             line[4] == 'i' &&
             line[5] == 'o' &&
             line[6] == 'n' &&
             line[7] == '\0') {
        console_info("Lumer OS version 0.1");
    }
    else if (line[0] == 'm' &&
             line[1] == 'e' &&
             line[2] == 'm' &&
             line[3] == '\0') {
        terminal_write("[INFO] Heap used: ");
        shell_print_uint(heap_used());
        terminal_write(" bytes");
        terminal_putchar('\n');
    }
    else if (line[0] == 'u' &&
             line[1] == 'p' &&
             line[2] == 't' &&
             line[3] == 'i' &&
             line[4] == 'm' &&
             line[5] == 'e' &&
             line[6] == '\0') {
        terminal_write("[INFO] Uptime: ");
        shell_print_uint(timer_ticks / 100);
        terminal_write(" seconds");
        terminal_putchar('\n');
    }
    else {
        console_warn("Unknown command");
    }

    shell_prompt();
}
