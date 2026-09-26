#include <stdint.h>
#include "paging.h"
#include "console.h"

/*
 * Day 25 paging configuration.
 *
 * Current memory layout:
 *
 *   Kernel:          around 0x10000
 *   TSS stack:       0x90000
 *   VGA:             0xB8000
 *   Heap:            0x200000 - 0x210000
 *   Page directory:  0x220000
 *   Page table:      0x221000
 *
 * The first 4 MiB is identity mapped, so every currently used
 * kernel address remains valid after paging is enabled.
 */

static uint32_t *page_directory =
    (uint32_t *)PAGE_DIRECTORY_ADDRESS;

static uint32_t *page_table =
    (uint32_t *)PAGE_TABLE_ADDRESS;

static volatile uint32_t paging_enabled = 0;

static inline uint32_t read_cr0(void)
{
    uint32_t value;

    __asm__ volatile (
        "mov %%cr0, %0"
        : "=r"(value)
    );

    return value;
}

static inline void write_cr0(uint32_t value)
{
    __asm__ volatile (
        "mov %0, %%cr0"
        :
        : "r"(value)
        : "memory"
    );
}

static inline void write_cr3(uint32_t value)
{
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(value)
        : "memory"
    );
}

static void paging_clear_structures(void)
{
    for (uint32_t i = 0; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
        page_table[i] = 0;
    }
}

static void paging_build_identity_map(void)
{
    page_directory[0] =
        PAGE_TABLE_ADDRESS |
        PAGE_PRESENT |
        PAGE_WRITE;

    for (uint32_t i = 0; i < PAGE_ENTRIES; i++) {
        uint32_t address = i * PAGE_SIZE;

        page_table[i] =
            address |
            PAGE_PRESENT |
            PAGE_WRITE;
    }
}

static int paging_verify_structures(void)
{
    if ((PAGE_DIRECTORY_ADDRESS & (PAGE_SIZE - 1U)) != 0)
        return 0;

    if ((PAGE_TABLE_ADDRESS & (PAGE_SIZE - 1U)) != 0)
        return 0;

    if ((page_directory[0] &
         (PAGE_PRESENT | PAGE_WRITE)) !=
        (PAGE_PRESENT | PAGE_WRITE))
        return 0;

    if (page_table[0] !=
        (0x00000000U | PAGE_PRESENT | PAGE_WRITE))
        return 0;

    if (page_table[1] !=
        (0x00001000U | PAGE_PRESENT | PAGE_WRITE))
        return 0;

    if (page_table[1023] !=
        (0x003FF000U | PAGE_PRESENT | PAGE_WRITE))
        return 0;

    return 1;
}

static void paging_enable(void)
{
    /*
     * CR3 must contain the physical address of the page directory.
     */
    write_cr3(PAGE_DIRECTORY_ADDRESS);

    /*
     * Enable the paging bit in CR0.
     */
    uint32_t cr0 = read_cr0();

    cr0 |= 0x80000000U;

    write_cr0(cr0);

    /*
     * MOV CR0 is serializing on x86. The following read confirms
     * that the PG bit is set.
     */
    cr0 = read_cr0();

    if ((cr0 & 0x80000000U) != 0)
        paging_enabled = 1;
    else
        paging_enabled = 0;
}

void paging_init(void)
{
    paging_enabled = 0;

    console_info("Paging: preparing structures...");

    paging_clear_structures();
    paging_build_identity_map();

    if (!paging_verify_structures()) {
        console_info("Paging structures: FAILED");
        return;
    }

    console_info("Paging structures: OK");
    console_info("Paging identity map: OK");

    console_info("Paging: loading CR3...");

    paging_enable();

    if (paging_enabled) {
        console_info("Paging enabled: YES");
    } else {
        console_info("Paging enabled: NO");
    }
}

uint32_t paging_get_directory(void)
{
    return PAGE_DIRECTORY_ADDRESS;
}

uint32_t paging_get_table(void)
{
    return PAGE_TABLE_ADDRESS;
}

int paging_is_enabled(void)
{
    return paging_enabled ? 1 : 0;
}
