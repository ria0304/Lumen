#include "shell.h"
#include "console.h"

static void shell_prompt(void)
{
    terminal_write("Lumer> ");
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

        console_info("Commands: help, clear, echo");
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
    else {
        console_warn("Unknown command");
    }

    shell_prompt();
}
