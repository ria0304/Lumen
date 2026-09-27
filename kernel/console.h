#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void terminal_clear(void);
void terminal_putchar(char c);
void terminal_write(const char *message);

/* Decimal rendering of an unsigned value, mirrored to serial like
 * everything else. Provided here so the self-test summary and the
 * shell agree on one formatter instead of each rolling their own. */
void terminal_write_u32(uint32_t value);

/* Two-digit uppercase hex, no leading "0x". Used for byte-level
 * diagnostics in self-tests. */
void terminal_write_hex8(uint8_t value);

void console_info(const char *message);
void console_warn(const char *message);
void console_error(const char *message);

#endif
