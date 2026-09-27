#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void shell_init(void);

/* Redraw the prompt. Used by the line editor after it takes over the
 * screen for Ctrl-L (clear) and Ctrl-C (abandon the line). */
void shell_print_prompt(void);
void shell_handle_line(const char *line);

/* Variable functions. */
const char *shell_get_var(const char *name);
int shell_set_var(const char *name, const char *value);
int shell_unset_var(const char *name);

/* String length helper. */
uint32_t shell_strlen(const char *s);

#endif
