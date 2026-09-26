#include <stdint.h>
#include "paging.h"
#include "console.h"

/*
 * Current Lumer memory layout:
 *
 *   Kernel:          around 0x10000
 *   TSS stack:       0x90000
 *   VGA:             0xB8000
 *   Heap:            0x200000 - 0x210000
 *   Page directory:  0x220000
 *   Page table:      0x221000
 *
 * Day 26 manages the existing first page table.
 *
 * This is deliberately limited to the first 4 MiB. A larger
 * multi-directory address space can be added in a later day.
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

static inline void invalidate_page(uint32_t address)
{
    __asm__ volatile (
        "invlpg (%0)"
        :
        : "r"(address)
        : "memory"
    );
}

static uint32_t page_index(uint32_t virtual_address)
{
    return (virtual_address >> 12) & 0x3FFU;
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
    write_cr3(PAGE_DIRECTORY_ADDRESS);

    uint32_t cr0 = read_cr0();

    cr0 |= 0x80000000U;

    write_cr0(cr0);

    cr0 = read_cr0();

    if ((cr0 & 0x80000000U) != 0)
        paging_enabled = 1;
    else
        paging_enabled = 0;
}

/*
 * Map one 4 KiB page in the first 4 MiB address space.
 *
 * Both addresses must be page aligned.
 */
int paging_map_page(
    uint32_t virtual_address,
    uint32_t physical_address,
    uint32_t flags
)
{
    if ((virtual_address & (PAGE_SIZE - 1U)) != 0)
        return -1;

    if ((physical_address & (PAGE_SIZE - 1U)) != 0)
        return -1;

    /*
     * Day 26 currently supports only the first page directory
     * entry, which covers virtual addresses 0x00000000-0x003FFFFF.
     */
    if (virtual_address >= 0x00400000U)
        return -1;

    uint32_t index = page_index(virtual_address);

    page_table[index] =
        (physical_address & 0xFFFFF000U) |
        (flags & 0xFFFU);

    if (paging_enabled)
        invalidate_page(virtual_address);

    return 0;
}

/*
 * Remove a 4 KiB mapping from the first page table.
 */
int paging_unmap_page(uint32_t virtual_address)
{
    if ((virtual_address & (PAGE_SIZE - 1U)) != 0)
        return -1;

    if (virtual_address >= 0x00400000U)
        return -1;

    uint32_t index = page_index(virtual_address);

    page_table[index] = 0;

    if (paging_enabled)
        invalidate_page(virtual_address);

    return 0;
}

/*
 * Return the physical address corresponding to a virtual address,
 * preserving the page offset.
 *
 * Returns 0xFFFFFFFF when the page is not mapped.
 */
uint32_t paging_get_mapping(uint32_t virtual_address)
{
    if (virtual_address >= 0x00400000U)
        return 0xFFFFFFFFU;

    uint32_t index = page_index(virtual_address);
    uint32_t entry = page_table[index];

    if ((entry & PAGE_PRESENT) == 0)
        return 0xFFFFFFFFU;

    return (entry & 0xFFFFF000U) |
           (virtual_address & 0x00000FFFU);
}

int paging_is_mapped(uint32_t virtual_address)
{
    return paging_get_mapping(virtual_address) != 0xFFFFFFFFU;
}

static int paging_self_test(void)
{
    /*
     * Check an existing identity mapping.
     */
    if (paging_get_mapping(0x00123000U) != 0x00123000U)
        return 0;

    /*
     * Check another identity mapping.
     */
    if (!paging_is_mapped(0x003FF000U))
        return 0;

    /*
     * Test remapping a page.
     *
     * Use 0x001FF000 because it is inside the existing identity
     * region but is not currently needed by the kernel startup
     * structures.
     */
    if (paging_map_page(
            0x001FF000U,
            0x001FE000U,
            PAGE_PRESENT | PAGE_WRITE
        ) != 0)
        return 0;

    if (paging_get_mapping(0x001FF000U) != 0x001FE000U)
        return 0;

    /*
     * Restore the original identity mapping.
     */
    if (paging_map_page(
            0x001FF000U,
            0x001FF000U,
            PAGE_PRESENT | PAGE_WRITE
        ) != 0)
        return 0;

    if (paging_get_mapping(0x001FF000U) != 0x001FF000U)
        return 0;

    /*
     * Test unmapping and remapping.
     */
    if (paging_unmap_page(0x001FE000U) != 0)
        return 0;

    if (paging_is_mapped(0x001FE000U))
        return 0;

    if (paging_map_page(
            0x001FE000U,
            0x001FE000U,
            PAGE_PRESENT | PAGE_WRITE
        ) != 0)
        return 0;

    if (!paging_is_mapped(0x001FE000U))
        return 0;

    /*
     * Verify invalid alignment is rejected.
     */
    if (paging_map_page(
            0x00123001U,
            0x00200000U,
            PAGE_PRESENT | PAGE_WRITE
        ) == 0)
        return 0;

    /*
     * Verify addresses beyond the first 4 MiB are rejected.
     */
    if (paging_map_page(
            0x00400000U,
            0x00400000U,
            PAGE_PRESENT | PAGE_WRITE
        ) == 0)
        return 0;

    return 1;
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

    if (!paging_enabled) {
        console_info("Paging enabled: NO");
        return;
    }

    console_info("Paging enabled: YES");

    if (!paging_self_test()) {
        console_info("Paging management test: FAILED");
        return;
    }

    console_info("Paging management test: PASS");
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
