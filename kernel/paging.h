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

/*
 * The master page directory and the single kernel page table.
 *
 * These must satisfy three constraints simultaneously:
 *
 *   1. Below PAGING_IDENTITY_LIMIT (4 MiB), because paging_init()
 *      identity-maps only the first 4 MiB and the CPU reads CR3
 *      through that map.
 *   2. Outside the kernel image, so the directory cannot be aliased
 *      by a static. It used to live at 0x00220000, which is inside
 *      .bss's address range -- harmless only because the arrays
 *      currently placed there stop short of it. linker.ld now
 *      asserts the kernel's .bss can never grow into 0x00100000.
 *   3. Inside a range the frame allocator already refuses to hand
 *      out or accept frees for, so no frame_alloc() can alias CR3's
 *      backing store. The 0x00100000-0x00200000 reservation in
 *      frame.c covers this.
 */
#define PAGE_DIRECTORY_ADDRESS 0x00100000U
#define PAGE_TABLE_ADDRESS     0x00101000U

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

/* Get physical address from a specific page directory (not current CR3) */
uint32_t paging_get_physical_from_directory(
    uint32_t directory_physical,
    uint32_t virtual_address
);

/*
 * Get the PTE flags for a virtual address in a specific page
 * directory. Returns 0 when the PDE or PTE is not present.
 */
uint32_t paging_flags_from_directory(
    uint32_t directory_physical,
    uint32_t virtual_address
);

uint32_t paging_directory_entries_used(void);

int paging_run_self_test(void);

/*
 * Per-address-space support: each Ring 3 task gets its own page
 * directory, created by paging_create_address_space() and
 * populated via paging_map_in_directory() *without* switching
 * CR3 to it first (safe only because the new directory's frame
 * is guaranteed to sit below PAGING_IDENTITY_LIMIT, so it can be
 * written to directly through the boot identity map).
 */
#define PAGING_IDENTITY_LIMIT 0x00400000U

/*
 * User virtual addresses live in [USER_ADDR_MIN, 0xC0000000), which
 * is page-directory entries USER_PDE_START..USER_PDE_LIMIT-1. Entry
 * USER_PDE_LIMIT and above belong to the kernel half and are shared,
 * never copied.
 *
 * The entries below USER_PDE_START are the kernel's own low identity
 * map (the first 4 MiB), which paging_create_address_space() copies in
 * along with everything else. They are not user pages, are not
 * private to the task, and must not be duplicated.
 */
#define USER_PDE_START 4U
#define USER_PDE_LIMIT 768U

uint32_t paging_create_address_space(void);

/*
 * Duplicate the user half of an address space into a fresh one, so a
 * forked task gets private copies of every page it can reach. Only
 * mappings below USER_PDE_LIMIT are copied; the kernel half is
 * inherited from the master directory by
 * paging_create_address_space() and must not be duplicated, or the
 * two tasks would end up with divergent copies of kernel memory.
 *
 * Writes a full copy (no copy-on-write): a fork is rare and the cost
 * is bounded by the task's own footprint.
 *
 * Returns 0 and stores the new directory in *out on success, -1 on
 * failure, having released everything it allocated.
 */
int paging_clone_address_space(
    uint32_t source_directory_physical,
    uint32_t *out
);

int paging_map_in_directory(
    uint32_t directory_physical,
    uint32_t virtual_address,
    uint32_t physical_address,
    uint32_t flags
);

void paging_destroy_address_space(
    uint32_t directory_physical
);

void paging_switch_directory(
    uint32_t directory_physical
);

uint32_t paging_current_directory(void);

#endif
