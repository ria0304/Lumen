#include <stdint.h>
#include "gdt.h"
#include "privilege.h"
#include "tss.h"

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
    if (gp.limit != sizeof(gdt) - 1)
        return 0;

    if (gp.base != (uint32_t)&gdt)
        return 0;

    if (gdt[0].access != 0)
        return 0;

    if (gdt[1].access != 0x9A)
        return 0;

    if (gdt[2].access != 0x92)
        return 0;

    if (gdt[3].access != 0xFA)
        return 0;

    if (gdt[4].access != 0xF2)
        return 0;

    if (gdt[5].access != 0x89)
        return 0;

    uint32_t tss_base =
        ((uint32_t)gdt[5].base_high << 24) |
        ((uint32_t)gdt[5].base_middle << 16) |
        gdt[5].base_low;

    if (tss_base != (uint32_t)&kernel_tss)
        return 0;

    return 1;
}
