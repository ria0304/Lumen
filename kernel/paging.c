#include <stdint.h>

#include "paging.h"
#include "frame.h"
#include "console.h"
#include "kmem.h"

static void print_hex32(uint32_t v){const char h[]="0123456789ABCDEF";terminal_write("0x");for(int i=7;i>=0;i--)terminal_putchar(h[(v>>(i*4))&0xF]);}

static uint32_t *const page_directory =
    (uint32_t *)PAGE_DIRECTORY_ADDRESS;

static uint32_t *const first_page_table =
    (uint32_t *)PAGE_TABLE_ADDRESS;

static volatile uint32_t paging_enabled;

static uint32_t active_directory = PAGE_DIRECTORY_ADDRESS;

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

uint32_t paging_flags_from_directory(
    uint32_t directory_physical,
    uint32_t virtual_address
)
{
    uint32_t pde_index = virtual_address >> 22;
    uint32_t pte_index = (virtual_address >> 12) & 0x3FFU;

    uint32_t *page_directory = (uint32_t *)directory_physical;
    uint32_t pde = page_directory[pde_index];

    if ((pde & PAGE_PRESENT) == 0)
        return 0;

    uint32_t page_table_phys = pde & 0xFFFFF000U;
    uint32_t *page_table = (uint32_t *)page_table_phys;

    uint32_t pte = page_table[pte_index];

    if ((pte & PAGE_PRESENT) == 0)
        return 0;

    return pte & 0xFFFU;
}

uint32_t paging_get_physical_from_directory(
    uint32_t directory_physical,
    uint32_t virtual_address
)
{
    uint32_t pde_index = virtual_address >> 22;
    uint32_t pte_index = (virtual_address >> 12) & 0x3FFU;

    /* Get the page directory entry */
    uint32_t *page_directory = (uint32_t *)directory_physical;
    uint32_t pde = page_directory[pde_index];

    if ((pde & PAGE_PRESENT) == 0)
        return 0;

    /* Get the page table */
    uint32_t page_table_phys = pde & 0xFFFFF000U;
    uint32_t *page_table = (uint32_t *)page_table_phys;

    uint32_t pte = page_table[pte_index];

    if ((pte & PAGE_PRESENT) == 0)
        return 0;

    return (pte & 0xFFFFF000U) + (virtual_address & (PAGE_SIZE - 1U));
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

static int map_in_inactive_directory(
    uint32_t dir_phys,
    uint32_t vaddr,
    uint32_t paddr,
    uint32_t flags
)
{
    if (dir_phys >= PAGING_IDENTITY_LIMIT)
        return -1;

    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t di = pd_index(vaddr);

    uint32_t *table;

    if (dir[di] & PAGE_PRESENT) {

        uint32_t table_phys =
            dir[di] & 0xFFFFF000U;

        if (table_phys >= PAGING_IDENTITY_LIMIT)
            return -1;

        table = (uint32_t *)table_phys;

    } else {

        uint32_t table_phys =
            frame_alloc();

        if (table_phys == FRAME_INVALID ||
            table_phys >= PAGING_IDENTITY_LIMIT)
            return -1;

        table = (uint32_t *)table_phys;

        for (uint32_t i = 0;
             i < PAGE_ENTRIES;
             ++i) {

            table[i] = 0;
        }

        dir[di] =
            table_phys |
            PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER;
    }

    uint32_t ti = pt_index(vaddr);

    table[ti] =
        (paddr & 0xFFFFF000U) |
        (flags & 0xFFFU) |
        PAGE_PRESENT;

    return 0;
}

/*
 * Allocate a fresh page directory and populate it with the same
 * kernel-space entries as the master directory (identity map,
 * heap, anything else already present). Those entries are shared
 * BY REFERENCE (same page table frame), not deep-copied -- future
 * kernel-space growth (e.g. the heap) stays visible automatically.
 * Only entries a task adds itself afterwards are private to it.
 */
uint32_t paging_create_address_space(void)
{
    uint32_t dir_phys =
        frame_alloc();

    if (dir_phys == FRAME_INVALID)
        return FRAME_INVALID;

    if (dir_phys >= PAGING_IDENTITY_LIMIT) {
        console_error(
            "Address space: directory frame out of range"
        );
        frame_free(dir_phys);
        return FRAME_INVALID;
    }

    uint32_t *dir =
        (uint32_t *)dir_phys;

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        dir[i] = page_directory[i];
    }

    return dir_phys;
}

/*
 * Map one page inside a directory that is NOT necessarily the
 * currently active one. Used to build a Ring 3 task's private
 * mappings before that task ever runs (and therefore before CR3
 * ever points at its directory).
 */
int paging_map_in_directory(
    uint32_t directory_physical,
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

    return map_in_inactive_directory(
        directory_physical,
        virtual_address,
        physical_address,
        flags
    );
}

/*
 * Free every frame private to this address space (page tables
 * and the pages they map that are NOT shared with the master
 * kernel directory), then free the directory itself. Shared
 * (kernel-space) entries are left completely untouched.
 */
/*
 * Duplicate the user half of an address space.
 *
 * paging_create_address_space() already copied the master directory,
 * so the new directory holds the correct kernel mappings. All that is
 * left is to give the child its own copy of every user page, so that
 * neither task can see the other's writes.
 *
 * Each page-directory entry needs both cases handled correctly:
 *
 *   PS = 0  the entry points at a page table. That table must be
 *           copied too, and every present PTE in it given its own
 *           frame. Copying the table's own frame as though it were a
 *           page looks like it works -- the child still resolves every
 *           address -- but both tasks then share the same physical
 *           frames, so writes cross between them.
 *
 *   PS = 1  the entry maps 4 MiB directly. Lumen never creates one of
 *           these for a user task, and copying it is not meaningful, so
 *           it is refused rather than silently mishandled.
 */
int paging_clone_address_space(
    uint32_t source_directory_physical,
    uint32_t *out
)
{
    if (out == 0 ||
        source_directory_physical == 0 ||
        source_directory_physical == PAGE_DIRECTORY_ADDRESS ||
        source_directory_physical >= PAGING_IDENTITY_LIMIT) {
        return -1;
    }

    uint32_t dst_phys = paging_create_address_space();

    if (dst_phys == FRAME_INVALID)
        return -1;

    uint32_t *src = (uint32_t *)source_directory_physical;
    uint32_t *dst = (uint32_t *)dst_phys;

    for (uint32_t pde = USER_PDE_START; pde < USER_PDE_LIMIT; pde++) {

        uint32_t entry = src[pde];

        if ((entry & PAGE_PRESENT) == 0)
            continue;

        if ((entry & 0x80U) != 0) {
            console_error("Address space: 4 MiB user page at PDE ");
            print_hex32(pde);
            terminal_putchar('\n');
            paging_destroy_address_space(dst_phys);
            return -1;
        }

        uint32_t src_table = entry & 0xFFFFF000U;
        uint32_t dst_table = frame_alloc();

        if (dst_table == FRAME_INVALID ||
            dst_table >= PAGING_IDENTITY_LIMIT) {

            if (dst_table != FRAME_INVALID)
                frame_free(dst_table);

            paging_destroy_address_space(dst_phys);
            return -1;
        }

        uint32_t *src_pt = (uint32_t *)src_table;
        uint32_t *dst_pt = (uint32_t *)dst_table;
        uint32_t dir_flags = entry & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

        for (uint32_t pte = 0; pte < PAGE_ENTRIES; pte++) {

            uint32_t page = src_pt[pte];

            if ((page & PAGE_PRESENT) == 0) {
                dst_pt[pte] = 0;
                continue;
            }

            uint32_t dst_frame = frame_alloc();

            if (dst_frame == FRAME_INVALID ||
                dst_frame >= PAGING_IDENTITY_LIMIT) {

                if (dst_frame != FRAME_INVALID)
                    frame_free(dst_frame);

                frame_free(dst_table);
                paging_destroy_address_space(dst_phys);
                return -1;
            }

            memcpy(
                (void *)dst_frame,
                (const void *)(page & 0xFFFFF000U),
                PAGE_SIZE
            );

            /* Keep the real flags (present/write/user) from the
             * source; the frame number is the only thing that changes.
             * The child is never read-only against the parent, so a
             * private copy does not need copy-on-write here. */
            dst_pt[pte] =
                dst_frame | (page & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER));
        }

        dst[pde] = dst_table | dir_flags;
    }

    *out = dst_phys;

    return 0;
}

void paging_destroy_address_space(
    uint32_t directory_physical
)
{
    if (directory_physical == 0 ||
        directory_physical == PAGE_DIRECTORY_ADDRESS ||
        directory_physical >= PAGING_IDENTITY_LIMIT)
        return;

    uint32_t *dir =
        (uint32_t *)directory_physical;

    for (uint32_t i = 0;
         i < PAGE_ENTRIES;
         ++i) {

        uint32_t entry = dir[i];

        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        uint32_t table_phys =
            entry & 0xFFFFF000U;

        uint32_t master_entry =
            page_directory[i];

        uint32_t master_table_phys =
            master_entry & 0xFFFFF000U;

        int private_table =
            !(master_entry & PAGE_PRESENT) ||
            (table_phys != master_table_phys);

        if (private_table &&
            table_phys < PAGING_IDENTITY_LIMIT) {

            uint32_t *table =
                (uint32_t *)table_phys;

            for (uint32_t j = 0;
                 j < PAGE_ENTRIES;
                 ++j) {

                if (table[j] & PAGE_PRESENT) {
                    frame_free(
                        table[j] & 0xFFFFF000U
                    );
                }
            }

            frame_free(table_phys);

            /*
             * Only clear the PDE for a table we actually just
             * freed. A shared (kernel) entry must be left alone:
             * if this directory is still the one loaded in CR3
             * (true when a Ring 3 task is torn down from inside
             * its own fault handler, before the scheduler has
             * switched away from it), zeroing a shared entry
             * unmaps the kernel code that is CURRENTLY RUNNING
             * this loop out from under itself, which page-faults
             * again immediately -- this time from Ring 0, which
             * is unrecoverable. The whole directory frame is
             * freed below regardless, so leaving a stale shared
             * entry in place until then is harmless.
             */
            dir[i] = 0;
        }
    }

    frame_free(directory_physical);
}

void paging_switch_directory(
    uint32_t directory_physical
)
{
    if (directory_physical == 0)
        directory_physical = PAGE_DIRECTORY_ADDRESS;

    if (directory_physical == active_directory)
        return;

    write_cr3(directory_physical);

    active_directory = directory_physical;
}

uint32_t paging_current_directory(void)
{
    return active_directory;
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
