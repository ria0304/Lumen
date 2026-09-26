#include <stdint.h>
#include "shell.h"
#include "ring3.h"
#include "console.h"
#include "heap.h"
#include "task.h"
#include "fs.h"
#include "loader.h"



extern volatile uint32_t timer_ticks;

static void shell_prompt(void)
{
    terminal_write("Lumer> ");
}

static uint32_t shell_parse_uint(const char *text)
{
    uint32_t value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (uint32_t)(*text - '0');
        text++;
    }

    return value;
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

        console_info("Commands: help, clear, echo, about, version, mem, uptime, task, taskkill, tasks");
        console_info("Filesystem: format, ls, cat <file>, write <file> <text>, rm <file>, run <file>");
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
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == '\0') {
        int id = task_create();

        if (id >= 0) {
            terminal_write("[INFO] Task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("Task creation failed");
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 'u' &&
             line[5] == 's' &&
             line[6] == 'e' &&
             line[7] == 'r' &&
             line[8] == '\0') {
        int id = task_create_with_privilege(USER_RING);

        if (id >= 0) {
            terminal_write("[INFO] User task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("User task creation failed");
        }
    }
    else if (line[0] == 'u' &&
             line[1] == 's' &&
             line[2] == 'e' &&
             line[3] == 'r' &&
             line[4] == 'm' &&
             line[5] == 'o' &&
             line[6] == 'd' &&
             line[7] == 'e' &&
             line[8] == '\0') {
        console_info("Entering user mode...");
        if (!ring3_init()) {
            console_error("Ring 3 initialization: FAIL");
        } else {
            console_info("Entering Ring 3...");
            __asm__ volatile ("call enter_user_mode");
        }
    }
    else if (string_starts_with(line, "taskkill ")) {
        uint32_t id = shell_parse_uint(line + 9);

        if (task_terminate(id) == 0) {
            terminal_write("[INFO] Task terminated: ID ");
            shell_print_uint(id);
            terminal_putchar('\n');
        } else {
            console_error("Task termination failed");
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 's' &&
             line[5] == '\0') {
        terminal_write("[INFO] Active tasks: ");
        shell_print_uint(task_count());
        terminal_putchar('\n');

        for (uint32_t id = 1; id < 100; id++) {
            const task_t *task = task_get(id);

            if (task != 0 &&
                task->state != TASK_UNUSED &&
                task->state != TASK_TERMINATED) {
                terminal_write("ID ");
                shell_print_uint(task->id);
                terminal_write(" READY");

                if (task->privilege == KERNEL_RING) {
                    terminal_write(" RING0");
                } else if (task->privilege == USER_RING) {
                    terminal_write(" RING3");
                }

                terminal_putchar('\n');
            }
        }
    }
    else if (line[0] == 'f' &&
             line[1] == 'o' &&
             line[2] == 'r' &&
             line[3] == 'm' &&
             line[4] == 'a' &&
             line[5] == 't' &&
             line[6] == '\0') {
        if (fs_format() == 0) {
            console_info("Disk formatted");
        } else {
            console_error("Format failed (is a disk attached?)");
        }
    }
    else if (line[0] == 'l' &&
             line[1] == 's' &&
             line[2] == '\0') {
        fs_list();
    }
    else if (string_starts_with(line, "cat ")) {
        const char *filename = line + 4;
        char buffer[513];
        int read = fs_read(filename, buffer, sizeof(buffer) - 1);

        if (read < 0) {
            console_error("No such file");
        } else {
            buffer[read] = '\0';
            terminal_write(buffer);
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "write ")) {
        const char *rest = line + 6;
        char filename[29];
        int i = 0;

        while (rest[i] != ' ' && rest[i] != '\0' && i < 28) {
            filename[i] = rest[i];
            i++;
        }
        filename[i] = '\0';

        const char *text = (rest[i] == ' ') ? rest + i + 1 : rest + i;
        int text_len = 0;

        while (text[text_len] != '\0')
            text_len++;

        if (i == 0 || text_len == 0) {
            console_error("Usage: write <file> <text>");
        } else if (fs_write(filename, text, (uint32_t)text_len) == 0) {
            console_info("File written");
        } else {
            console_error("Write failed (no disk, full, or too large)");
        }
    }
    else if (string_starts_with(line, "rm ")) {
        const char *filename = line + 3;

        if (fs_delete(filename) == 0) {
            console_info("File deleted");
        } else {
            console_error("No such file");
        }
    }
    else if (string_starts_with(line, "run ")) {
        const char *filename = line + 4;
        int id = loader_spawn(filename);

        if (id >= 0) {
            terminal_write("[INFO] Spawned task ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        }
    }
    else {
        console_warn("Unknown command");
    }

    shell_prompt();
}
