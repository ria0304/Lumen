#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);
int gdt_run_self_test(void);

#endif
