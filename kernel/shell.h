#ifndef SHELL_H
#define SHELL_H

void shell_init(void);

/* Redraw the prompt. Used by the line editor after it takes over the
 * screen for Ctrl-L (clear) and Ctrl-C (abandon the line). */
void shell_print_prompt(void);
void shell_handle_line(const char *line);

#endif
