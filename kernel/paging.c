#include <stdint.h>
#include "paging.h"
#include "console.h"

/*
 * Day 25:
 * Paging infrastructure only.
 *
 * The page directory and page table are placed above the current
 * 64 KiB kernel heap:
 *
 *   Heap:          0x200000 - 0x210000
 *   Paging memory: 0x220000 - 0x222000
 *
 * Both structures are inside the first 4 MiB identity-mapped region
 * that will be used when paging is eventually enabled.
 */

static uint32_t *page_directory =
    (uint32_t *)PAGE_DIRECTORY_ADDRESS;

static uint32_t *page_table =
    (uint32_t *)PAGE_TABLE_ADDRESS;

static volatile uint32_t paging_enabled = 0;

static void paging_clear_structures(void)
{
    for (uint32_t i = 0; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
        page_table[i] = 0;
    }
}

static void paging_build_identity_map(void)
{
    /*
     * First page directory entry points to our single page table.
     */
    page_directory[0] =
        PAGE_TABLE_ADDRESS |
        PAGE_PRESENT |
        PAGE_WRITE;

    /*
     * Identity-map the first 4 MiB:
     *
     * virtual address == physical address
     *
     * This covers the current kernel, VGA memory, TSS stack,
     * heap, task stacks and paging structures.
     */
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

void paging_init(void)
{
    paging_enabled = 0;

    paging_clear_structures();
    paging_build_identity_map();

    if (!paging_verify_structures()) {
        console_info("Paging structures: FAILED");
        return;
    }

    console_info("Paging structures: OK");
    console_info("Paging identity map: OK");
    console_info("Paging enabled: NO");

    /*
     * CR3/CR0.PG are intentionally NOT modified on Day 25.
     *
     * This gives us a verified paging foundation without changing
     * the currently working execution environment.
     */
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
