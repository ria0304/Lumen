#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/*
 * COM1 (0x3F8) output, mirrored from every character the VGA
 * console writes.
 *
 * This exists so the kernel's behaviour can be verified without
 * looking at a VGA screen: 'make test' runs QEMU headless with
 * -serial stdio and greps the output. Every self-test therefore
 * has to report through the same terminal_* path, and it lands
 * here automatically.
 */

void serial_init(void);

/* 0 until serial_init() finds a working UART. All output is
 * silently dropped before that, so callers never need to care. */
int serial_is_ready(void);

void serial_putchar(char c);
void serial_write(const char *message);

/*
 * Print an unsigned value in decimal. Used for the self-test
 * counters and for any number that needs to appear in serial
 * output without pulling in the shell's formatter.
 */
void serial_print_uint(uint32_t value);

/*
 * Shut the machine down under QEMU by writing to the isa-debug-exit
 * port, which QEMU turns into a process exit with status (code<<1)|1.
 * Under real hardware this is a no-op.
 */
void qemu_exit(int code);

#endif
