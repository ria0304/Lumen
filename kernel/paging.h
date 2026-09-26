#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096U
#define PAGE_ENTRIES    1024U

#define PAGE_PRESENT    0x001U
#define PAGE_WRITE      0x002U
#define PAGE_USER       0x004U

#define PAGE_DIRECTORY_ADDRESS 0x220000U
#define PAGE_TABLE_ADDRESS     0x221000U

void paging_init(void);
uint32_t paging_get_directory(void);
uint32_t paging_get_table(void);
int paging_is_enabled(void);

#endif
