#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void terminal_clear(void);
void terminal_putchar(char c);
void terminal_write(const char *message);

void console_info(const char *message);
void console_warn(const char *message);
void console_error(const char *message);

#endif
