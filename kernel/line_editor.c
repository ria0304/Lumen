#include <stdint.h>
#include "line_editor.h"
#include "console.h"
#include "shell.h"
#include "keycodes.h"

static char line_buffer[LINE_BUFFER_SIZE];
static char submitted_buffer[LINE_BUFFER_SIZE];

/* Number of characters currently in the line, and where the cursor
 * sits within them. The cursor is a byte offset, so it can be moved
 * without rewriting the buffer. */
static uint32_t line_length = 0;
static uint32_t cursor = 0;

static char history[LINE_HISTORY_SIZE][LINE_BUFFER_SIZE];
static uint32_t history_count = 0;

/* Index of the entry the cursor is currently sitting on while
 * browsing. Equals history_count when not browsing, i.e. when the
 * line is the user's own. */
static uint32_t history_browse = 0;

/* The line as it was before history recall started, so moving back
 * off the end of the history restores what the user was typing. */
static char history_saved[LINE_BUFFER_SIZE];
static uint32_t history_saved_length = 0;

void line_editor_init(void)
{
    line_length = 0;
    cursor = 0;
    line_buffer[0] = '\0';
    submitted_buffer[0] = '\0';
    history_count = 0;
    history_browse = 0;
    history_saved[0] = '\0';
    history_saved_length = 0;
}

/* Redraw the line from the cursor's new position to the end, leaving
 * the cursor where the caller put it. */
static void refresh_from_cursor(void)
{
    for (uint32_t i = cursor; i < line_length; i++)
        terminal_putchar(line_buffer[i]);

    /*
     * One extra backspace parks the hardware cursor on top of the
     * character that was just overwritten, or just past the end of
     * the line if the cursor is at the end.
     */
    terminal_putchar('\b');

    for (uint32_t i = cursor; i < line_length; i++)
        terminal_putchar(line_buffer[i]);
}

static void redraw_line(void)
{
    for (uint32_t i = 0; i < line_length; i++)
        terminal_putchar(line_buffer[i]);

    for (uint32_t i = line_length; i < cursor; i++)
        terminal_putchar('\b');
}

static void clear_line(void)
{
    for (uint32_t i = 0; i < line_length; i++)
        terminal_putchar('\b');

    for (uint32_t i = 0; i < line_length; i++)
        terminal_putchar(' ');

    for (uint32_t i = 0; i < line_length; i++)
        terminal_putchar('\b');

    line_length = 0;
    cursor = 0;
    line_buffer[0] = '\0';
}

static void insert_char(char c)
{
    if (line_length >= LINE_BUFFER_SIZE - 1)
        return;

    for (uint32_t i = line_length; i > cursor; i--)
        line_buffer[i] = line_buffer[i - 1];

    line_buffer[cursor] = c;
    line_length++;
    cursor++;

    terminal_putchar(c);
    refresh_from_cursor();
}

static void delete_forward(void)
{
    if (cursor >= line_length)
        return;

    for (uint32_t i = cursor; i + 1 < line_length; i++)
        line_buffer[i] = line_buffer[i + 1];

    line_length--;
    line_buffer[line_length] = '\0';

    /*
     * Erase the character that moved into the cursor's place, then
     * put the rest of the line back.
     */
    terminal_putchar(' ');
    refresh_from_cursor();
}

static void history_add(const char *line)
{
    if (line[0] == '\0')
        return;

    /* Repeating the previous entry would just fill the ring with
     * duplicates, so skip an exact repeat of the newest entry. */
    if (history_count > 0) {
        const char *newest = history[history_count - 1];

        uint32_t i = 0;

        while (line[i] != '\0' &&
               newest[i] == line[i])
            i++;

        if (line[i] == '\0' && newest[i] == '\0')
            return;
    }

    if (history_count == LINE_HISTORY_SIZE) {

        for (uint32_t i = 1; i < LINE_HISTORY_SIZE; i++) {

            for (uint32_t j = 0; j < LINE_BUFFER_SIZE; j++)
                history[i - 1][j] = history[i][j];
        }

        history_count--;
    }

    uint32_t i = 0;

    while (line[i] != '\0' && i < LINE_BUFFER_SIZE - 1) {
        history[history_count][i] = line[i];
        i++;
    }

    history[history_count][i] = '\0';
    history_count++;
}

/* Load a history entry into the editing buffer. direction is -1 for
 * older (up arrow) and +1 for newer (down arrow). */
static void history_recall(int direction)
{
    if (history_count == 0)
        return;

    if (history_browse == history_count) {

        /* Starting a browse: stash whatever is being typed. */
        uint32_t i = 0;

        while (i < line_length && i < LINE_BUFFER_SIZE - 1) {
            history_saved[i] = line_buffer[i];
            i++;
        }

        history_saved[i] = '\0';
        history_saved_length = line_length;
    }

    int64_t target =
        (int64_t)history_browse + direction;

    if (target < 0)
        target = 0;

    if (target > (int64_t)history_count)
        target = (int64_t)history_count;

    history_browse = (uint32_t)target;

    clear_line();

    const char *entry;

    if (history_browse == history_count)
        entry = history_saved;
    else
        entry = history[history_browse];

    uint32_t i = 0;

    while (entry[i] != '\0' && i < LINE_BUFFER_SIZE - 1) {
        line_buffer[i] = entry[i];
        i++;
    }

    line_buffer[i] = '\0';
    line_length = i;
    cursor = i;

    terminal_write(line_buffer);
}

static void submit(void)
{
    terminal_putchar('\n');

    for (uint32_t i = 0; i <= line_length; i++)
        submitted_buffer[i] = line_buffer[i];

    history_add(line_buffer);

    shell_handle_line(submitted_buffer);

    line_length = 0;
    cursor = 0;
    line_buffer[0] = '\0';
    history_browse = history_count;
    history_saved[0] = '\0';
    history_saved_length = 0;
}

void line_editor_handle_char(uint8_t key)
{
    /* Anything with the high bit set is a KEY_* control code, never a
     * character to insert. */
    if (key >= 0x80) {

        switch (key) {
            case KEY_LEFT:
                if (cursor > 0) {
                    cursor--;
                    terminal_putchar('\b');
                }
                break;

            case KEY_RIGHT:
                if (cursor < line_length) {
                    terminal_putchar(line_buffer[cursor]);
                    cursor++;
                }
                break;

            case KEY_HOME:
                while (cursor > 0) {
                    cursor--;
                    terminal_putchar('\b');
                }
                break;

            case KEY_END:
                while (cursor < line_length) {
                    terminal_putchar(line_buffer[cursor]);
                    cursor++;
                }
                break;

            case KEY_DELETE:
                delete_forward();
                break;

            case KEY_UP:
                history_recall(-1);
                break;

            case KEY_DOWN:
                history_recall(1);
                break;

            case KEY_PAGE_UP:
            case KEY_PAGE_DOWN:
                break;

            case KEY_CTRL_U:
                clear_line();
                break;

            case KEY_CTRL_L:
                terminal_clear();
                shell_print_prompt();
                redraw_line();
                break;

            case KEY_CTRL_C:
                /* Abandon the line without running it, the way a
                 * real shell treats an interrupt at the prompt. */
                clear_line();
                terminal_putchar('^');
                terminal_putchar('C');
                shell_print_prompt();
                break;

            default:
                break;
        }

        return;
    }

    if (key == '\b') {

        if (cursor > 0) {

            for (uint32_t i = cursor; i < line_length; i++)
                line_buffer[i - 1] = line_buffer[i];

            line_length--;
            line_buffer[line_length] = '\0';
            cursor--;

            terminal_putchar('\b');
            refresh_from_cursor();
        }

        return;
    }

    if (key == '\n') {
        submit();
        return;
    }

    if (key == '\t' || key == KEY_TAB) {
        /* Tab completion. */
        if (cursor == line_length) {
            /* Find the start of the current word. */
            uint32_t word_start = cursor;
            while (word_start > 0 && line_buffer[word_start - 1] != ' ' && line_buffer[word_start - 1] != '\t') {
                word_start--;
            }

            /* Extract the current word prefix. */
            uint32_t prefix_len = cursor - word_start;
            if (prefix_len > 0) {
                char prefix[LINE_BUFFER_SIZE];
                for (uint32_t i = 0; i < prefix_len; i++) {
                    prefix[i] = line_buffer[word_start + i];
                }
                prefix[prefix_len] = '\0';

                /* Find commands that match the prefix. */
                static const char *builtins[] = {
                    "help", "clear", "echo", "about", "version", "mem", "uptime",
                    "task", "taskkill", "tasks", "ps", "taskuser", "wait",
                    "vmtest", "privtest", "exittest",
                    "install", "run", "exittest",
                    "format", "ls", "cat", "write", "rm", "storage-test", "diskinfo", "date",
                    "ln", "readlink", "mv", "df", "mkdir", "rmdir", "install",
                    "format", "ls", "cat", "write", "rm", "storage-test", "diskinfo", "date",
                    "ln", "readlink", "mv", "df", "mkdir", "rmdir", "install",
                    "format", "ls", "cat", "write", "rm", "storage-test", "diskinfo", "date",
                    "ln", "readlink", "mv", "df", "mkdir", "rmdir", "install",
                    "format", "ls", "cat", "write", "rm", "storage-test", "diskinfo", "date",
                    "fg", "bg", "jobs", "wait", "taskkill", "taskuser",
                    0
                };

                int match_count = 0;
                const char *match_str = 0;
                for (int i = 0; builtins[i]; i++) {
                    const char *b = builtins[i];
                    int match = 1;
                    for (uint32_t k = 0; k < prefix_len; k++) {
                        if (b[k] != prefix[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        match_count++;
                        match_str = b;
                    }
                }

                if (match_count == 1 && match_str) {
                    /* Single match - complete it. */
                    uint32_t suffix_len = 0;
                    while (match_str[suffix_len] && suffix_len < LINE_BUFFER_SIZE - line_length - 1) {
                        suffix_len++;
                    }
                    if (suffix_len > prefix_len) {
                        /* Complete the word. */
                        for (uint32_t i = prefix_len; i < suffix_len; i++) {
                            insert_char(match_str[i]);
                        }
                        /* Add a space after completion. */
                        insert_char(' ');
                    } else if (match_count > 1) {
                        /* Multiple matches - show them. */
                        terminal_putchar('\n');
                        for (int i = 0; builtins[i]; i++) {
                            const char *b = builtins[i];
                            int match = 1;
                            for (uint32_t k = 0; k < prefix_len; k++) {
                                if (b[k] != prefix[k]) {
                                    match = 0;
                                    break;
                                }
                            }
                            if (match) {
                                terminal_write(b);
                                terminal_putchar(' ');
                            }
                        }
                        terminal_putchar('\n');
                        shell_print_prompt();
                        redraw_line();
                    }
                }
        }
        return;
    }
    }  // Missing closing brace for line_editor_handle_char

    if (key < 0x20)
        return;

    insert_char((char)key);
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
