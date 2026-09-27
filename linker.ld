ENTRY(_start)

/*
 * __bss_start / __bss_end let kernel/entry.asm zero .bss before any C
 * runs. A growing amount of kernel state lives there -- the frame
 * bitmap, the heap block headers, the task table -- and none of it
 * may depend on whatever the boot sector happened to leave in memory
 * after loading the image.
 */
SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    __bss_start = .;

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    __bss_end = .;

    /*
     * The master page directory lives at 0x00100000 (see
     * PAGE_DIRECTORY_ADDRESS in kernel/paging.h). It has to stay
     * outside the kernel image or a static could be aliased by the
     * table the CPU is actively translating every memory access
     * through. Failing the build here is much cheaper than finding
     * this as a mystery corruption after some unrelated change
     * grows .bss past the mark.
     */
    ASSERT(__bss_end <= 0x00100000,
           "kernel .bss collides with the master page directory at 0x00100000 -- move PAGE_DIRECTORY_ADDRESS up in kernel/paging.h")
}
