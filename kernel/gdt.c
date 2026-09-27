#include <stdint.h>
#include "gdt.h"
#include "privilege.h"
#include "tss.h"
#include "console.h"

extern tss_t kernel_tss;

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gp;

static void gdt_set_gate(
    int number,
    uint32_t base,
    uint32_t limit,
    uint8_t access,
    uint8_t granularity
)
{
    gdt[number].base_low = base & 0xFFFF;
    gdt[number].base_middle = (base >> 16) & 0xFF;
    gdt[number].base_high = (base >> 24) & 0xFF;

    gdt[number].limit_low = limit & 0xFFFF;
    gdt[number].granularity =
        ((limit >> 16) & 0x0F) |
        (granularity & 0xF0);

    gdt[number].access = access;
}

extern void gdt_flush(uint32_t gp);

void gdt_init(void)
{
    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel code: Ring 0 */
    gdt_set_gate(
        1, 0, 0xFFFFF,
        0x9A, 0xCF
    );

    /* Kernel data: Ring 0 */
    gdt_set_gate(
        2, 0, 0xFFFFF,
        0x92, 0xCF
    );

    /* User code: Ring 3 */
    gdt_set_gate(
        3, 0, 0xFFFFF,
        0xFA, 0xCF
    );

    /* User data: Ring 3 */
    gdt_set_gate(
        4, 0, 0xFFFFF,
        0xF2, 0xCF
    );

    /* 32-bit available TSS */
    gdt_set_gate(
        5,
        (uint32_t)&kernel_tss,
        sizeof(tss_t) - 1,
        0x89,
        0x00
    );

    gdt_flush((uint32_t)&gp);
}

int gdt_run_self_test(void)
{
    /*
     * Access-byte comparisons mask off the bits the CPU owns, since
     * they change in the table behind the CPU's back:
     *
     *   - Code/data segments get the Accessed flag (bit 0) set the
     *     first time a segment register is loaded with that
     *     selector, so the kernel data descriptor reads back as
     *     0x93 rather than the 0x92 written at init.
     *   - A 32-bit TSS descriptor changes type from 9 (available) to
     *     11 (busy) when 'ltr' loads it, i.e. bit 1 gets set, so it
     *     reads back as 0x8B rather than 0x89. Bit 0 is part of
     *     that type encoding already, not an accessed flag.
     *
     * Comparing the raw bytes made this self-test report failures
     * for a GDT that was in fact correct.
     */
    #define GDT_ACCESS_MASK 0xFE
    #define GDT_TSS_BUSY_MASK 0xFD

    /*
     * Each check reports which expectation broke rather than just
     * returning 0, so a failure here is diagnosable from the serial
     * log instead of being an anonymous "GDT self-test: FAILED".
     */
    if (gp.limit != sizeof(gdt) - 1) {
        console_error("GDT self-test: gp.limit wrong");
        return 0;
    }

    if (gp.base != (uint32_t)&gdt) {
        console_error("GDT self-test: gp.base wrong");
        return 0;
    }

    if (gdt[0].access != 0) {
        console_error("GDT self-test: null descriptor not empty");
        return 0;
    }

    if ((gdt[1].access & GDT_ACCESS_MASK) != 0x9A) {
        console_error("GDT self-test: kernel code access byte wrong");
        return 0;
    }

    if ((gdt[2].access & GDT_ACCESS_MASK) != 0x92) {
        console_error("GDT self-test: kernel data access byte wrong");
        return 0;
    }

    if ((gdt[3].access & GDT_ACCESS_MASK) != 0xFA) {
        console_error("GDT self-test: user code access byte wrong");
        return 0;
    }

    if ((gdt[4].access & GDT_ACCESS_MASK) != 0xF2) {
        console_error("GDT self-test: user data access byte wrong");
        return 0;
    }

    if ((gdt[5].access & GDT_TSS_BUSY_MASK) != 0x89) {
        console_error("GDT self-test: TSS access byte = 0x");
        terminal_write_hex8(gdt[5].access);
        return 0;
    }

    uint32_t tss_base =
        ((uint32_t)gdt[5].base_high << 24) |
        ((uint32_t)gdt[5].base_middle << 16) |
        gdt[5].base_low;

    if (tss_base != (uint32_t)&kernel_tss) {
        console_error("GDT self-test: TSS base wrong");
        return 0;
    }

    /*
     * A Ring 3 task is only reachable if the user descriptors are
     * actually loaded into the CPU, so confirm the reload took by
     * reading back the live segment registers.
     */
    uint32_t cs = 0;
    uint32_t ds = 0;

    __asm__ volatile (
        "mov %%cs, %0\n"
        "mov %%ds, %1\n"
        : "=r"(cs), "=r"(ds)
    );

    if ((cs & 0xFFFF) != KERNEL_CODE_SELECTOR) {
        console_error("GDT self-test: live CS is not the kernel code selector");
        return 0;
    }

    if ((ds & 0xFFFF) != KERNEL_DATA_SELECTOR) {
        console_error("GDT self-test: live DS is not the kernel data selector");
        return 0;
    }

    return 1;
}
