#include <stdint.h>

#include "paging.h"
#include "frame.h"
#include "console.h"

static uint32_t *const page_directory =
    (uint32_t *)PAGE_DIRECTORY_ADDRESS;

static uint32_t *const first_page_table =
    (uint32_t *)PAGE_TABLE_ADDRESS;

static volatile uint32_t paging_enabled;

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

static inline uint32_t pd_index(uint32_t address)
{
    return address >> 22;
}

static inline uint32_t pt_index(uint32_t address)
{
    return (address >> 12) & 0x3FFU;
}

static uint32_t *page_table_for(uint32_t virtual_address)
{
    uint32_t pde =
        page_directory[pd_index(virtual_address)];

    if ((pde & PAGE_PRESENT) == 0)
        return 0;

    return (uint32_t *)(pde & 0xFFFFF000U);
}

static int ensure_page_table(
    uint32_t virtual_address,
    uint32_t flags
)
{
    uint32_t directory_index =
        pd_index(virtual_address);

    uint32_t pde =
        page_directory[directory_index];

    if (pde & PAGE_PRESENT) {

        if ((flags & PAGE_USER) &&
            !(pde & PAGE_USER)) {

            page_directory[directory_index] |=
                PAGE_USER;
        }

        if ((flags & PAGE_WRITE) &&
            !(pde & PAGE_WRITE)) {

            page_directory[directory_index] |=
                PAGE_WRITE;
        }

        return 0;
    }

    uint32_t table_frame =
        frame_alloc();

    if (table_frame == FRAME_INVALID)
        return -1;

    uint32_t *table =
        (uint32_t *)table_frame;

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        table[i] = 0;
    }

    page_directory[directory_index] =
        table_frame |
        PAGE_PRESENT |
        PAGE_WRITE |
        ((flags & PAGE_USER)
            ? PAGE_USER
            : 0);

    if (paging_enabled)
        write_cr3(PAGE_DIRECTORY_ADDRESS);

    return 0;
}

static void release_empty_page_table(
    uint32_t directory_index
)
{
    if (directory_index == 0)
        return;

    uint32_t pde =
        page_directory[directory_index];

    if (!(pde & PAGE_PRESENT))
        return;

    uint32_t *table =
        (uint32_t *)(pde & 0xFFFFF000U);

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        if (table[i] & PAGE_PRESENT)
            return;
    }

    uint32_t table_frame =
        pde & 0xFFFFF000U;

    page_directory[directory_index] = 0;

    frame_free(table_frame);

    if (paging_enabled)
        write_cr3(PAGE_DIRECTORY_ADDRESS);
}

static void clear_structures(void)
{
    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        page_directory[i] = 0;
        first_page_table[i] = 0;
    }
}

static void build_identity_map(void)
{
    page_directory[0] =
        PAGE_TABLE_ADDRESS |
        PAGE_PRESENT |
        PAGE_WRITE;

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        uint32_t address =
            i * PAGE_SIZE;

        first_page_table[i] =
            address |
            PAGE_PRESENT |
            PAGE_WRITE;
    }
}

static int verify_initial_map(void)
{
    if ((PAGE_DIRECTORY_ADDRESS &
         (PAGE_SIZE - 1U)) != 0)
        return 0;

    if ((PAGE_TABLE_ADDRESS &
         (PAGE_SIZE - 1U)) != 0)
        return 0;

    if (page_directory[0] !=
        (PAGE_TABLE_ADDRESS |
         PAGE_PRESENT |
         PAGE_WRITE))
        return 0;

    if (first_page_table[0] !=
        (PAGE_PRESENT |
         PAGE_WRITE))
        return 0;

    if (first_page_table[1023] !=
        (0x003FF000U |
         PAGE_PRESENT |
         PAGE_WRITE))
        return 0;

    return 1;
}

static void enable_paging(void)
{
    write_cr3(PAGE_DIRECTORY_ADDRESS);

    uint32_t cr0 =
        read_cr0();

    cr0 |= 0x80000000U;

    write_cr0(cr0);

    paging_enabled =
        (read_cr0() &
         0x80000000U) != 0;
}

int paging_map_page(
    uint32_t virtual_address,
    uint32_t physical_address,
    uint32_t flags
)
{
    if ((virtual_address &
         (PAGE_SIZE - 1U)) != 0)
        return -1;

    if ((physical_address &
         (PAGE_SIZE - 1U)) != 0)
        return -1;

    if (physical_address >=
        FRAME_MEMORY_LIMIT)
        return -1;

    if (ensure_page_table(
            virtual_address,
            flags) != 0)
        return -1;

    uint32_t *table =
        page_table_for(virtual_address);

    if (!table)
        return -1;

    uint32_t index =
        pt_index(virtual_address);

    if (table[index] & PAGE_PRESENT)
        return -1;

    table[index] =
        physical_address |
        (flags & 0xFFFU) |
        PAGE_PRESENT;

    invalidate_page(
        virtual_address
    );

    return 0;
}

int paging_unmap_page(
    uint32_t virtual_address
)
{
    if ((virtual_address &
         (PAGE_SIZE - 1U)) != 0)
        return -1;

    uint32_t directory_index =
        pd_index(virtual_address);

    uint32_t *table =
        page_table_for(virtual_address);

    if (!table)
        return -1;

    uint32_t index =
        pt_index(virtual_address);

    if (!(table[index] & PAGE_PRESENT))
        return -1;

    table[index] = 0;

    invalidate_page(
        virtual_address
    );

    release_empty_page_table(
        directory_index
    );

    return 0;
}

uint32_t paging_get_mapping(
    uint32_t virtual_address
)
{
    uint32_t *table =
        page_table_for(virtual_address);

    if (!table)
        return FRAME_INVALID;

    uint32_t entry =
        table[pt_index(virtual_address)];

    if (!(entry & PAGE_PRESENT))
        return FRAME_INVALID;

    return entry & 0xFFFFF000U;
}

uint32_t paging_get_flags(
    uint32_t virtual_address
)
{
    uint32_t *table =
        page_table_for(virtual_address);

    if (!table)
        return 0;

    return table[
        pt_index(virtual_address)
    ] & 0xFFFU;
}

int paging_is_mapped(
    uint32_t virtual_address
)
{
    return paging_get_mapping(
        virtual_address
    ) != FRAME_INVALID;
}

uint32_t paging_alloc_page(
    uint32_t virtual_address,
    uint32_t flags
)
{
    uint32_t physical =
        frame_alloc();

    if (physical == FRAME_INVALID)
        return FRAME_INVALID;

    if (paging_map_page(
            virtual_address,
            physical,
            flags) != 0) {

        frame_free(physical);
        return FRAME_INVALID;
    }

    return physical;
}

int paging_free_page(
    uint32_t virtual_address
)
{
    uint32_t physical =
        paging_get_mapping(
            virtual_address
        );

    if (physical == FRAME_INVALID)
        return -1;

    if (paging_unmap_page(
            virtual_address
        ) != 0)
        return -1;

    return frame_free(
        physical
    );
}

int paging_alloc_zero_page(
    uint32_t virtual_address,
    uint32_t flags
)
{
    uint32_t physical =
        paging_alloc_page(
            virtual_address,
            flags
        );

    if (physical == FRAME_INVALID)
        return -1;

    uint8_t *memory =
        (uint8_t *)virtual_address;

    for (uint32_t i = 0;
         i < PAGE_SIZE;
         ++i) {

        memory[i] = 0;
    }

    return 0;
}

uint32_t paging_virtual_to_physical(
    uint32_t virtual_address
)
{
    uint32_t physical =
        paging_get_mapping(
            virtual_address
        );

    if (physical == FRAME_INVALID)
        return FRAME_INVALID;

    return physical +
        (virtual_address &
         (PAGE_SIZE - 1U));
}

uint32_t paging_directory_entries_used(void)
{
    uint32_t count = 0;

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        if (page_directory[i] &
            PAGE_PRESENT)
            ++count;
    }

    return count;
}

int paging_run_self_test(void)
{
    /*
     * Test outside the initial 4 MiB identity map.
     * This forces creation of a dynamic page table.
     */
    uint32_t virtual_address =
        0x00C00000U;

    uint32_t physical =
        paging_alloc_page(
            virtual_address,
            PAGE_WRITE
        );

    if (physical == FRAME_INVALID)
        return 0;

    if (!paging_is_mapped(
            virtual_address))
        return 0;

    if (paging_get_mapping(
            virtual_address) != physical)
        return 0;

    if (paging_virtual_to_physical(
            virtual_address + 123) !=
        physical + 123)
        return 0;

    if (paging_free_page(
            virtual_address) != 0)
        return 0;

    if (paging_is_mapped(
            virtual_address))
        return 0;

    console_info(
        "Paging dynamic memory test: PASS"
    );

    return 1;
}

void paging_init(void)
{
    clear_structures();

    build_identity_map();

    if (!verify_initial_map()) {
        console_error(
            "Paging structures: FAILED"
        );
        return;
    }

    console_info(
        "Paging structures: OK"
    );

    console_info(
        "Paging identity map: OK"
    );

    console_info(
        "Paging: loading CR3..."
    );

    enable_paging();

    if (!paging_enabled) {
        console_error(
            "Paging enabled: NO"
        );
        return;
    }

    console_info(
        "Paging enabled: YES"
    );
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
