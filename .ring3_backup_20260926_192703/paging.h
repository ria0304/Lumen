#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096U
#define PAGE_ENTRIES    1024U

#define PAGE_PRESENT    0x001U
#define PAGE_WRITE      0x002U
#define PAGE_USER       0x004U

#define PAGE_WRITETHROUGH   0x008U
#define PAGE_CACHE_DISABLE  0x010U
#define PAGE_ACCESSED       0x020U
#define PAGE_DIRTY          0x040U
#define PAGE_GLOBAL         0x100U

#define PAGE_DIRECTORY_ADDRESS 0x00220000U
#define PAGE_TABLE_ADDRESS     0x00221000U

#define PAGE_DIRECTORY_COUNT   1024U
#define PAGE_TABLE_SPAN        0x00400000U

void paging_init(void);

uint32_t paging_get_directory(void);
uint32_t paging_get_table(void);
int paging_is_enabled(void);

int paging_map_page(
    uint32_t virtual_address,
    uint32_t physical_address,
    uint32_t flags
);

int paging_unmap_page(
    uint32_t virtual_address
);

uint32_t paging_get_mapping(
    uint32_t virtual_address
);

uint32_t paging_get_flags(
    uint32_t virtual_address
);

int paging_is_mapped(
    uint32_t virtual_address
);

uint32_t paging_alloc_page(
    uint32_t virtual_address,
    uint32_t flags
);

int paging_free_page(
    uint32_t virtual_address
);

int paging_alloc_zero_page(
    uint32_t virtual_address,
    uint32_t flags
);

uint32_t paging_virtual_to_physical(
    uint32_t virtual_address
);

uint32_t paging_directory_entries_used(void);

int paging_run_self_test(void);

#endif
