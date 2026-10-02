#include <stdint.h>
#include "gdt.h"
#include "tss.h"
#include "scheduler.h"
#include "paging.h"
#include "frame.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "heap.h"
#include "console.h"
#include "line_editor.h"
#include "shell.h"
#include "task.h"
#include "privilege.h"
#include "ata.h"
#include "rtc.h"
#include "fs.h"
#include "gui.h"
#include "settings.h"
#include "users.h"
#include "sha256.h"
#include "net.h"
#include "pkg.h"
#include "proc.h"
#include "klog.h"
#include "cron.h"
#include "gfx.h"
#include "nic.h"
#include "uaccess.h"
#include "ring3test.h"
#include "version.h"
#include "serial.h"

extern void kbd_init(void);

#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

volatile uint32_t timer_ticks = 0;

static uint32_t self_tests_passed = 0;
static uint32_t self_tests_failed = 0;

/*
 * Single reporting point for every boot self-test. The uniform
 * "SELFTEST <label> PASS/FAIL" line is what 'make test' greps for
 * on the serial mirror, so the format here is load-bearing: keep the
 * prefix and the PASS/FAIL words exactly as they are.
 *
 * Every *_run_self_test() in this codebase follows the shell's
 * convention: 0 means a check failed, non-zero means it passed.
 */
static void report_self_test(const char *label, int rc)
{
    terminal_write("SELFTEST ");
    terminal_write(label);
    terminal_write(rc != 0 ? " PASS\n" : " FAIL\n");

    if (rc != 0)
        self_tests_passed++;
    else
        self_tests_failed++;
}

static void print_hex32(uint32_t value)
{
    const char hex[] = "0123456789ABCDEF";

    terminal_write("0x");

    for (int i = 7; i >= 0; i--) {
        uint8_t digit =
            (value >> (i * 4)) & 0xF;

        terminal_putchar(hex[digit]);
    }
}

static void task_fault_recover(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void exception_handler(
    uint32_t vector,
    uint32_t error_code,
    uint32_t *frame
)
{
    console_error("CPU EXCEPTION RECEIVED");

    terminal_write("[ERROR] Vector: ");
    print_hex32(vector);
    terminal_putchar('\n');

    terminal_write("[ERROR] Error code: ");
    print_hex32(error_code);
    terminal_putchar('\n');

    /* Where it happened. The frame is the interrupted context the
     * stub pushed, so saved_eip is the faulting instruction. */
    if (frame != 0) {
        terminal_write("[ERROR] EIP: ");
        print_hex32(frame[/* saved_eip */ 8]);
        terminal_putchar('\n');

        terminal_write("[ERROR] EBP: ");
        print_hex32(frame[/* saved_ebp */ 5]);
        terminal_putchar('\n');

        terminal_write("[ERROR] ESP: ");
        print_hex32(frame[/* saved_esp */ 3]);
        terminal_putchar('\n');
    }

    if (vector == 14) {
        uint32_t cr2;

        __asm__ volatile (
            "mov %%cr2, %0"
            : "=r"(cr2)
        );

        terminal_write("[ERROR] Fault address: ");
        print_hex32(cr2);
        terminal_putchar('\n');
    }

    switch (vector) {
        case 0:  console_error("Exception 0: Divide by zero"); break;
        case 1:  console_error("Exception 1: Debug"); break;
        case 2:  console_error("Exception 2: Non-maskable interrupt"); break;
        case 3:  console_error("Exception 3: Breakpoint"); break;
        case 4:  console_error("Exception 4: Overflow"); break;
        case 5:  console_error("Exception 5: Bound range exceeded"); break;
        case 6:  console_error("Exception 6: Invalid opcode"); break;
        case 7:  console_error("Exception 7: Device not available"); break;
        case 8:  console_error("Exception 8: Double fault"); break;
        case 9:  console_error("Exception 9: Coprocessor segment overrun"); break;
        case 10: console_error("Exception 10: Invalid TSS"); break;
        case 11: console_error("Exception 11: Segment not present"); break;
        case 12: console_error("Exception 12: Stack-segment fault"); break;
        case 13: console_error("Exception 13: General protection fault"); break;
        case 14: console_error("Exception 14: Page fault"); break;
        case 15: console_error("Exception 15: Reserved"); break;
        case 16: console_error("Exception 16: x87 floating-point"); break;
        case 17: console_error("Exception 17: Alignment check"); break;
        case 18: console_error("Exception 18: Machine check"); break;
        case 19: console_error("Exception 19: SIMD floating-point"); break;
        case 20: console_error("Exception 20: Virtualization"); break;
        case 21: console_error("Exception 21: Control protection"); break;
        default: console_error("Exception: Reserved/unknown CPU exception"); break;
    }

    /*
     * A fault from Ring 3 (CPL == 3 in the saved CS) is the
     * misbehaving program's problem, not the kernel's. For a
     * deliberately conservative set of recoverable vectors,
     * terminate the current task (freeing its address space)
     * and redirect the SAME interrupt frame this handler is
     * about to return through into a trivial kernel-mode halt
     * loop, instead of back into the faulting Ring 3 code. The
     * next timer tick then schedules a different, still-healthy
     * task away from it -- the same recovery path already used
     * when a task yields, blocks, or exits normally. Genuine
     * kernel-mode (Ring 0) faults are untouched by this and
     * still halt exactly as before.
     */
    if ((frame[1] & 0x3U) == 0x3U) {

        int recoverable =
            (vector == 0)  ||
            (vector == 4)  ||
            (vector == 5)  ||
            (vector == 6)  ||
            (vector == 12) ||
            (vector == 13) ||
            (vector == 14);

        if (recoverable) {

            uint32_t id =
                scheduler_current_task();

            terminal_write(
                "[ERROR] Ring 3 task faulted, terminating task: "
            );
            print_hex32(id);
            terminal_putchar('\n');

            task_terminate(id);

            frame[0] =
                (uint32_t)task_fault_recover;

            frame[1] =
                KERNEL_CODE_SELECTOR;

            frame[2] |=
                0x200U;

            return;
        }
    }

    console_error("System halted");

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void irq_unhandled_handler(uint32_t irq)
{
    /*
     * A spurious interrupt has no cause to report, and acknowledging
     * it would corrupt the PIC's in-service bookkeeping. pic_end_of_
     * interrupt() already knows to skip the EOI in that case.
     */
    if (pic_is_spurious((uint8_t)irq)) {
        pic_end_of_interrupt((uint8_t)irq);
        return;
    }

    terminal_write("[WARN] Unhandled hardware IRQ: ");
    print_hex32(irq);
    terminal_putchar('\n');

    pic_end_of_interrupt((uint8_t)irq);
}

void timer_handler(void)
{
    timer_ticks++;
    cron_tick(timer_ticks);
    if (gfx_is_active()) { pic_end_of_interrupt(0); return; }

    /*
     * Display the low 16 bits of the timer tick count
     * in hexadecimal at the top-right of the screen.
     */

    if (timer_ticks % 100 == 0) {
        volatile unsigned short *seconds_display =
            (volatile unsigned short *)VGA_MEMORY;

        const char text[] = "SEC:";
        for (int i = 0; i < 4; i++) {
            seconds_display[80 + 70 + i] =
                ((unsigned short)VGA_COLOR << 8) | text[i];
        }

        uint32_t seconds = timer_ticks / 100;
        const char hex[] = "0123456789ABCDEF";

        for (int i = 0; i < 4; i++) {
            uint8_t digit = seconds & 0xF;
            seconds_display[80 + 78 - i] =
                ((unsigned short)VGA_COLOR << 8) | hex[digit];
            seconds >>= 4;
        }
    }
    volatile unsigned short *timer_display =
        (volatile unsigned short *)VGA_MEMORY;

    const char hex[] = "0123456789ABCDEF";
    uint32_t value = timer_ticks;

    for (int i = 0; i < 8; i++) {
        uint8_t digit = value & 0xF;
        timer_display[79 - i] =
            ((unsigned short)VGA_COLOR << 8) | hex[digit];
        value >>= 4;
    }

    /*
     * End of interrupt for IRQ0. Sent last so the scheduler work
     * above completes before another timer interrupt can arrive.
     */
    pic_end_of_interrupt(0);
}

void kmain(void)
{
    /*
     * Serial first, before a single character is printed: every
     * later line is mirrored here, and 'make test' reads nothing
     * else.
     *
     * The single raw 'M' is a temporary probe proving kmain is
     * entered at all, independent of the serial driver's state.
     */
    __asm__ volatile ("outb %%al, %%dx" : : "a"((char)'M'), "d"((unsigned short)0x3F8));

    serial_init();

    /*
     * The IDT goes up immediately after serial, and that ordering is
     * load-bearing.
     *
     * The boot sector hands us protected mode with the BIOS's
     * real-mode IVT still loaded: base 0x0000, limit 0x03FF. Until
     * idt_init() runs, every CPU exception therefore dispatches
     * through physical address 0, which is a table of leftover BIOS
     * interrupt handlers living at f000:xxxx. A fault in that window
     * does not crash visibly -- it jumps the CPU into BIOS code, which
     * eventually returns through a corrupted stack and lands back in
     * entry.asm's halt loop with no output at all. That is exactly how
     * this kernel used to fail: no serial output, EIP inside f000:xxxx
     * at the halt, and a stack pointer that had wandered above the
     * 0x90000 stack.
     *
     * Installing the IDT second means every subsequent init step --
     * terminal_clear, tss_init, gdt_init, frame_init, paging_init -- is
     * covered by real handlers that print the vector, error code and
     * CR2 fault address, so a mistake reports itself instead of
     * vanishing.
     *
     * Safe to do this before gdt_init(): idt_init() only references
     * KERNEL_CODE_SEGMENT (0x08), and the boot GDT's index 1 is
     * byte-identical to the kernel GDT's index 1 (0xFFFF / 0x9A /
     * 0xCF), so the gates resolve correctly against both. tss_load()
     * still has to wait for gdt_init(), because the TSS gate (0x28)
     * exists only in the kernel GDT.
     */
    idt_init();

    terminal_clear();

    tss_init();
    gdt_init();
    tss_load();

    console_info(LUMEN_BANNER);
    console_info("VGA text driver: OK");
    console_info("Protected mode: 32-bit");

    report_self_test("GDT", gdt_run_self_test());
    report_self_test("TSS", tss_run_self_test());

    report_self_test("IDT", idt_run_self_test());


    /*
     * The paging subsystem allocates dynamic page-table frames,
     * so the physical frame allocator must be initialized first.
     */
    frame_init();

    report_self_test("FRAME", frame_run_self_test());

    paging_init();

    report_self_test("PAGING", paging_run_self_test());

    console_info("IDT initialized: OK");

    pic_init();
    console_info("PIC initialized: OK");

    pit_init(100);
    console_info("PIT initialized: 100 Hz");

    kbd_init();
    console_info("Keyboard initialized: OK");

    line_editor_init();
    console_info("Line editor initialized: OK");

    shell_init();

    task_init();

    report_self_test("TASK", task_run_self_test());

    scheduler_init();

    report_self_test("SCHEDULER", scheduler_run_self_test());

    report_self_test("SYSCALL", syscall_run_self_test());

    heap_init();
    console_info("Memory allocator: OK");

    ata_init();
    fs_init();

    if (ata_is_ready()) {
        report_self_test("ATA", ata_run_self_test());
    } else {
        terminal_write("SELFTEST ATA SKIP (no drive)\n");
    }

    report_self_test("RTC", rtc_run_self_test());

    /*
     * The filesystem self-test is only meaningful on a formatted
     * disk, and an unformatted one is a normal first-boot state
     * rather than a failure -- so it is skipped, not failed, when
     * there is nothing to test.
     */
    if (fs_is_mounted()) {
        report_self_test("FS", fs_self_test());

        /*
         * Runs after the filesystem self-test, and only once there is
         * a disk to work on: this one drives real file I/O from a
         * Ring 3 task, so it exercises paging, uaccess, the fd table
         * and LumenFS together.
         */
        report_self_test("RING3IO", ring3_io_run_self_test());
        report_self_test("RING3FLT", ring3_fault_run_self_test());
        report_self_test("RING3FRK", ring3_fork_run_self_test());
        report_self_test("RING3EXE", ring3_exec_run_self_test());
        report_self_test("RING3SYC", ring3_syscalls_run_self_test());
        report_self_test("RING3ELF", ring3_elf_run_self_test());
    } else {
        terminal_write("SELFTEST FS SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3IO SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3FLT SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3FRK SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3EXE SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3SYC SKIP (unformatted)\n");
        terminal_write("SELFTEST RING3ELF SKIP (unformatted)\n");
        console_warn("FS: run 'format' to initialize the disk");
    }

    report_self_test("HEAP", heap_run_self_test());
    report_self_test("GUI", gui_run_self_test());
    settings_init();
    report_self_test("SETTINGS", settings_run_self_test());
    users_init();
    report_self_test("USERS", users_run_self_test());
    net_init();
    report_self_test("NET", net_run_self_test());
    pkg_init();
    report_self_test("PKG", pkg_run_self_test());
    report_self_test("PROC", proc_run_self_test());
    klog_put("lumen boot ok");
    report_self_test("KLOG", klog_run_self_test());
    report_self_test("CRON", cron_run_self_test());
    report_self_test("GFX", gfx_run_self_test());
    nic_init();
    report_self_test("NIC", nic_run_self_test());

    char *buffer = (char *)kmalloc(64);

    if (buffer != 0) {
        const char message[] = "Dynamic buffer allocation: WORKING";

        int i = 0;
        while (message[i] != '\0') {
            buffer[i] = message[i];
            i++;
        }
        buffer[i] = '\0';

        console_info(buffer);
        console_info("Heap allocation test passed");

        uint32_t used = heap_used();
        char digits[10];
        int count = 0;

        while (used > 0) {
            digits[count++] = '0' + (used % 10);
            used /= 10;
        }

        if (count == 0)
            terminal_putchar('0');

        while (count > 0)
            terminal_putchar(digits[--count]);

        terminal_write(" bytes\n");
    } else {
        console_error("Memory allocation: FAILED");
    }

    console_info("Enabling timer + keyboard interrupts...");

    /*
     * One machine-readable summary line for the whole boot phase.
     * 'make test' asserts on this and on the individual
     * "SELFTEST <label> PASS" lines above it.
     */
    terminal_write("SELFTEST-SUMMARY pass=");
    terminal_write_u32(self_tests_passed);
    terminal_write(" fail=");
    terminal_write_u32(self_tests_failed);
    terminal_write("\n");

    console_info("Boot self-tests complete");

    __asm__ volatile ("sti");

#ifdef LUMEN_AUTOEXIT
    /*
     * Built only by 'make test'. Shut the machine down instead of
     * idling so the harness does not have to wait on a timeout,
     * and so QEMU's exit status carries the result.
     */
    qemu_exit(self_tests_failed == 0 ? 0 : 1);
#endif

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
