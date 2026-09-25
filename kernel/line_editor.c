#include <stdint.h>
#include "line_editor.h"
#include "console.h"

static char line_buffer[LINE_BUFFER_SIZE];
static char submitted_buffer[LINE_BUFFER_SIZE];
static uint32_t line_length = 0;

void line_editor_init(void)
{
    line_length = 0;
    line_buffer[0] = '\0';
    submitted_buffer[0] = '\0';
}

void line_editor_handle_char(char c)
{
    if (c == '\b') {
        if (line_length > 0) {
            line_length--;
            line_buffer[line_length] = '\0';
            terminal_putchar('\b');
        }

        return;
    }

    if (c == '\n') {
        terminal_putchar('\n');

        for (uint32_t i = 0; i <= line_length; i++)
            submitted_buffer[i] = line_buffer[i];

        line_length = 0;
        line_buffer[0] = '\0';
        return;
    }

    if (c == '\t')
        return;

    if (line_length >= LINE_BUFFER_SIZE - 1)
        return;

    line_buffer[line_length++] = c;
    line_buffer[line_length] = '\0';

    terminal_putchar(c);
}

const char *line_editor_get_buffer(void)
{
    return line_buffer;
}

uint32_t line_editor_length(void)
{
    return line_length;
}

const char *line_editor_get_submitted(void)
{
    return submitted_buffer;
}
