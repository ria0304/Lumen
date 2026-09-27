#include <stdint.h>
#include "shell.h"
#include "ring3.h"
#include "console.h"
#include "heap.h"
#include "task.h"
#include "fs.h"
#include "ata.h"
#include "rtc.h"
#include "loader.h"



extern volatile uint32_t timer_ticks;

/*
 * Ring 3 VM protection test.
 *
 * 0x00001000 belongs to the kernel's supervisor-only
 * identity mapping. Ring 3 must NOT be able to read it.
 *
 * mov eax, [0x1000]
 * jmp $
 */
static const uint8_t vm_fault_program[] = {
    0xA1, 0x00, 0x10, 0x00, 0x00,
    0xEB, 0xFE
};

/*
 * Ring 3 privilege protection test.
 *
 * CLI is a privileged instruction and must raise #GP
 * when executed at CPL 3.
 *
 * cli
 * jmp $
 */
static const uint8_t privilege_fault_program[] = {
    0xFA,
    0xEB, 0xFE
};


static void shell_prompt(void)
{
    terminal_write("Lumer> ");
}

/*
 * Public so the line editor can redraw the prompt after a Ctrl-L
 * clear or a Ctrl-C abandon, when it owns the screen and cannot let
 * shell_handle_line() print one for it.
 */
void shell_print_prompt(void)
{
    shell_prompt();
}

static uint32_t shell_strlen(const char *s)
{
    uint32_t n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/* Render a directory, one entry per line, with a trailing slash on
 * directories so the listing is unambiguous. */
static void shell_list(const char *path)
{
    if (!fs_is_mounted()) {
        console_error("No filesystem mounted (run 'format')");
        return;
    }

    uint32_t count = 0;
    int rc = fs_count_entries(path, FS_ROOT, &count);

    if (rc != FS_OK) {
        console_error("ls: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return;
    }

    if (count == 0) {
        console_info("(empty)");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ino = 0;
        const char *name = 0;
        uint8_t type = 0;

        if (fs_list(path, FS_ROOT, i, &ino, &name, &type) != FS_OK)
            break;

        terminal_write(name);

        if (type == FS_TYPE_DIR) {
            terminal_putchar('/');
        } else {
            /* Show the size, which is the thing that actually
             * distinguishes two files. */
            terminal_write("  ");
            fs_inode_t meta;

            if (fs_stat_by_inode(ino, &meta) == FS_OK)
                terminal_write_u32(meta.size);

            terminal_write(" B");
        }

        terminal_putchar('\n');
    }
}

static uint32_t shell_parse_uint(const char *text)
{
    uint32_t value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (uint32_t)(*text - '0');
        text++;
    }

    return value;
}

static void shell_print_uint(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0) {
        digits[count++] = '0' + (value % 10);
        value /= 10;
    }

    while (count > 0)
        terminal_putchar(digits[--count]);
}

static int string_starts_with(const char *text, const char *prefix)
{
    int i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i])
            return 0;

        i++;
    }

    return 1;
}

void shell_init(void)
{
    shell_prompt();
}

void shell_handle_line(const char *line)
{
    if (line[0] == '\0') {
        shell_prompt();
        return;
    }

    if (line[0] == 'h' &&
        line[1] == 'e' &&
        line[2] == 'l' &&
        line[3] == 'p' &&
        line[4] == '\0') {

        console_info("Commands: help, clear, echo, about, version, mem, uptime, task, taskkill, tasks, ps, taskuser, wait <pid>");
        console_info("Protection: vmtest, privtest");
        console_info("Programs: install, run <file>, exittest");
        console_info("Storage: format, ls, cat <file>, write <file> <text>, rm <file>, storage-test, diskinfo, date");
    }
    else if (string_starts_with(line, "echo ")) {
        terminal_write(line + 5);
        terminal_putchar('\n');
    }
    else if (line[0] == 'c' &&
             line[1] == 'l' &&
             line[2] == 'e' &&
             line[3] == 'a' &&
             line[4] == 'r' &&
             line[5] == '\0') {
        terminal_clear();
    }
    else if (line[0] == 'a' &&
             line[1] == 'b' &&
             line[2] == 'o' &&
             line[3] == 'u' &&
             line[4] == 't' &&
             line[5] == '\0') {
        console_info("Lumer OS.");
        console_info("Built from scratch in C and x86 assembly.");
        console_info("Custom bootloader, kernel, interrupts, memory, keyboard, and shell.");
    }
    else if (line[0] == 'v' &&
             line[1] == 'e' &&
             line[2] == 'r' &&
             line[3] == 's' &&
             line[4] == 'i' &&
             line[5] == 'o' &&
             line[6] == 'n' &&
             line[7] == '\0') {
        console_info("Lumer OS version 0.1");
    }
    else if (line[0] == 'm' &&
             line[1] == 'e' &&
             line[2] == 'm' &&
             line[3] == '\0') {
        terminal_write("[INFO] Heap used: ");
        shell_print_uint(heap_used());
        terminal_write(" bytes");
        terminal_putchar('\n');
    }
    else if (line[0] == 'u' &&
             line[1] == 'p' &&
             line[2] == 't' &&
             line[3] == 'i' &&
             line[4] == 'm' &&
             line[5] == 'e' &&
             line[6] == '\0') {
        terminal_write("[INFO] Uptime: ");
        shell_print_uint(timer_ticks / 100);
        terminal_write(" seconds");
        terminal_putchar('\n');
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == '\0') {
        int id = task_create();

        if (id >= 0) {
            terminal_write("[INFO] Task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("Task creation failed");
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 'u' &&
             line[5] == 's' &&
             line[6] == 'e' &&
             line[7] == 'r' &&
             line[8] == '\0') {
        int id = task_create_with_privilege(USER_RING);

        if (id >= 0) {
            terminal_write("[INFO] User task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("User task creation failed");
        }
    }
    else if (line[0] == 'v' &&
             line[1] == 'm' &&
             line[2] == 't' &&
             line[3] == 'e' &&
             line[4] == 's' &&
             line[5] == 't' &&
             line[6] == '\0') {

        int id =
            task_create_user_program(
                vm_fault_program,
                sizeof(vm_fault_program)
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] VM protection test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');

            console_info(
                "Expected: Ring 3 page fault"
            );
        } else {
            console_error(
                "VM protection test creation failed"
            );
        }
    }
    else if (line[0] == 'p' &&
             line[1] == 'r' &&
             line[2] == 'i' &&
             line[3] == 'v' &&
             line[4] == 't' &&
             line[5] == 'e' &&
             line[6] == 's' &&
             line[7] == 't' &&
             line[8] == '\0') {

        int id =
            task_create_user_program(
                privilege_fault_program,
                sizeof(privilege_fault_program)
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] Privilege protection test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');

            console_info(
                "Expected: Ring 3 general protection fault"
            );
        } else {
            console_error(
                "Privilege protection test creation failed"
            );
        }
    }
    else if (line[0] == 'e' &&
             line[1] == 'x' &&
             line[2] == 'i' &&
             line[3] == 't' &&
             line[4] == 't' &&
             line[5] == 'e' &&
             line[6] == 's' &&
             line[7] == 't' &&
             line[8] == '\0') {

        int id =
            task_create_user_program(
                ring3_exit_program,
                ring3_exit_program_size
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] Exit test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("Exit test task creation failed");
        }
    }
    else if (line[0] == 'u' &&
             line[1] == 's' &&
             line[2] == 'e' &&
             line[3] == 'r' &&
             line[4] == 'm' &&
             line[5] == 'o' &&
             line[6] == 'd' &&
             line[7] == 'e' &&
             line[8] == '\0') {
        console_info("Entering user mode...");
        if (!ring3_init()) {
            console_error("Ring 3 initialization: FAIL");
        } else {
            console_info("Entering Ring 3...");
            __asm__ volatile ("call enter_user_mode");
        }
    }
    else if (string_starts_with(line, "taskkill ")) {
        uint32_t id = shell_parse_uint(line + 9);

        if (task_terminate(id) == 0) {
            terminal_write("[INFO] Task terminated: ID ");
            shell_print_uint(id);
            terminal_putchar('\n');
        } else {
            console_error("Task termination failed");
        }
    }
    else if (line[0] == 'p' &&
             line[1] == 's' &&
             line[2] == '\0') {

        terminal_write("PID PPID STATE RING\n");

        for (uint32_t id = 0; id <= MAX_TASKS; id++) {
            const task_t *task = task_get(id);

            if (task == 0)
                continue;

            terminal_write(" ");
            shell_print_uint(task->id);
            terminal_write("   ");
            shell_print_uint(task->parent_id);
            terminal_write("   ");

            if (task->state == TASK_RUNNING) {
                terminal_write("RUNNING");
            } else if (task->state == TASK_READY) {
                terminal_write("READY");
            } else if (task->state == TASK_BLOCKED) {
                terminal_write("BLOCKED");
            } else if (task->state == TASK_TERMINATED) {
                terminal_write("TERMINATED");
            } else {
                terminal_write("UNUSED");
            }

            terminal_write(" ");

            if (task->privilege == KERNEL_RING) {
                terminal_write("0");
            } else {
                terminal_write("3");
            }

            terminal_putchar('\n');
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 's' &&
             line[5] == '\0') {
        terminal_write("[INFO] Active tasks: ");
        shell_print_uint(task_count());
        terminal_putchar('\n');

        for (uint32_t id = 1; id < 100; id++) {
            const task_t *task = task_get(id);

            if (task != 0 &&
                task->state != TASK_UNUSED &&
                task->state != TASK_TERMINATED) {
                terminal_write("ID ");
                shell_print_uint(task->id);
                terminal_write(" READY");

                if (task->privilege == KERNEL_RING) {
                    terminal_write(" RING0");
                } else if (task->privilege == USER_RING) {
                    terminal_write(" RING3");
                }

                terminal_putchar('\n');
            }
        }
    }
    else if (string_starts_with(line, "wait ")) {
        uint32_t id = shell_parse_uint(line + 5);

        int result = task_wait(id);

        if (result >= 0) {
            terminal_write("[INFO] Collected child task ID ");
            shell_print_uint((uint32_t)result);
            terminal_putchar('\n');
        } else {
            console_error("Wait failed (not a terminated child)");
        }
    }
    else if (string_starts_with(line, "diskinfo")) {
        if (!ata_is_ready()) {
            console_error("No ATA drive present");
        } else {
            terminal_write("[INFO] Model:  ");
            terminal_write(ata_model()[0] ? ata_model() : "(unreported)");
            terminal_putchar('\n');

            terminal_write("[INFO] Serial: ");
            terminal_write(ata_serial()[0] ? ata_serial() : "(unreported)");
            terminal_putchar('\n');

            terminal_write("[INFO] Capacity: ");
            shell_print_uint((uint32_t)(ata_total_bytes() / 1024U / 1024U));
            terminal_write(" MiB (");
            shell_print_uint((uint32_t)ata_total_sectors());
            terminal_write(" sectors, 512 B each)\n");

            if (ata_requires_lba48()) {
                console_warn("Drive needs LBA48; only the first 128 GiB are addressable");
            }
        }
    }
    else if (string_starts_with(line, "date")) {
        rtc_time_t now;

        if (rtc_read(&now) != 0) {
            console_warn("Hardware clock unavailable; showing fallback date");
        }

        char stamp[32];
        rtc_format(&now, stamp, sizeof(stamp));
        terminal_write(stamp);
        terminal_putchar('\n');
    }
    else if (string_starts_with(line, "storage-test")) {
        if (!fs_is_mounted()) {
            console_error("Storage test requires a formatted filesystem");
        } else if (fs_self_test() != 0) {
            console_error("Storage test failed");
        }
    }
    else if (line[0] == 'f' &&
             line[1] == 'o' &&
             line[2] == 'r' &&
             line[3] == 'm' &&
             line[4] == 'a' &&
             line[5] == 't' &&
             line[6] == '\0') {
        int rc = fs_format(0, 0);

        if (rc == FS_OK) {
            console_info("Disk formatted as LumenFS v2");
        } else {
            console_error("Format failed: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (line[0] == 'l' &&
             line[1] == 's' &&
             line[2] == '\0') {
        shell_list("/");
    }
    else if (string_starts_with(line, "ls ")) {
        const char *path = line + 3;

        while (*path == ' ')
            path++;

        shell_list(*path == '\0' ? "/" : path);
    }
    else if (string_starts_with(line, "cat ")) {
        const char *filename = line + 4;

        while (*filename == ' ')
            filename++;

        if (*filename == '\0') {
            console_error("Usage: cat <file>");
        } else {
            void *buffer = 0;
            uint32_t size = 0;
            int rc = fs_read(filename, FS_ROOT, &buffer, &size);

            if (rc != FS_OK) {
                console_error("cat: ");
                terminal_write(fs_strerror(rc));
                terminal_putchar('\n');
            } else {
                if (size > 0) {
                    terminal_write((const char *)buffer);

                    if (((const char *)buffer)[size - 1] != '\n')
                        terminal_putchar('\n');
                }

                kfree(buffer);
            }
        }
    }
    else if (string_starts_with(line, "write ")) {
        const char *rest = line + 6;
        const char *space = 0;

        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                space = p;
                break;
            }
        }

        if (space == 0 || space == rest || space[1] == '\0') {
            console_error("Usage: write <file> <text>");
        } else {
            char filename[FS_NAME_MAX + 1];
            uint32_t name_len = (uint32_t)(space - rest);
            int rc;

            if (name_len > FS_NAME_MAX) {
                console_error("write: name too long");
            } else {
                for (uint32_t i = 0; i < name_len; i++)
                    filename[i] = rest[i];

                filename[name_len] = '\0';

                rc = fs_write(filename, FS_ROOT, space + 1,
                              shell_strlen(space + 1));

                if (rc == FS_OK) {
                    console_info("Wrote ");
                    terminal_write(filename);
                } else {
                    console_error("Write failed: ");
                    terminal_write(fs_strerror(rc));
                    terminal_putchar('\n');
                }
            }
        }
    }
    else if (string_starts_with(line, "rm ")) {
        const char *filename = line + 3;
        int rc = fs_delete(filename, FS_ROOT);

        if (rc == FS_OK) {
            console_info("File deleted");
        } else {
            console_error("rm: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "mkdir ")) {
        const char *path = line + 6;

        while (*path == ' ')
            path++;

        int rc = fs_mkdir(path, FS_ROOT, FS_MODE_DIR_DEFAULT);

        if (rc == FS_OK) {
            console_info("Directory created");
        } else {
            console_error("mkdir: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "rmdir ")) {
        const char *path = line + 6;

        while (*path == ' ')
            path++;

        int rc = fs_rmdir(path, FS_ROOT);

        if (rc == FS_OK) {
            console_info("Directory removed");
        } else {
            console_error("rmdir: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "mv ")) {
        const char *rest = line + 3;

        while (*rest == ' ')
            rest++;

        const char *to = 0;

        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                to = p + 1;
                break;
            }
        }

        if (to == 0) {
            console_error("Usage: mv <from> <to>");
        } else {
            char from[FS_PATH_MAX];
            uint32_t from_len = (uint32_t)(to - 1 - rest);

            if (from_len >= FS_PATH_MAX) {
                console_error("mv: path too long");
            } else {
                for (uint32_t i = 0; i < from_len; i++)
                    from[i] = rest[i];

                from[from_len] = '\0';

                int rc = fs_rename(from, to, FS_ROOT);

                if (rc == FS_OK) {
                    console_info("Renamed");
                } else {
                    console_error("mv: ");
                    terminal_write(fs_strerror(rc));
                    terminal_putchar('\n');
                }
            }
        }
    }
    else if (string_starts_with(line, "df")) {
        if (!fs_is_mounted()) {
            console_error("No filesystem mounted");
        } else {
            terminal_write("[INFO] Total: ");
            terminal_write_u32((uint32_t)(fs_total_bytes() / 1024U));
            terminal_write(" KiB, free: ");
            terminal_write_u32((uint32_t)(fs_free_bytes() / 1024U));
            terminal_write(" KiB\n");
        }
    }
    else if (line[0] == 'i' &&
             line[1] == 'n' &&
             line[2] == 's' &&
             line[3] == 't' &&
             line[4] == 'a' &&
             line[5] == 'l' &&
             line[6] == 'l' &&
             line[7] == '\0') {

        if (fs_write(
                "hello.bin",
                FS_ROOT,
                ring3_test_program,
                ring3_test_program_size
            ) == FS_OK) {

            console_info("Installed hello.bin");
        } else {
            console_error("Failed to install hello.bin");
        }
    }
    else if (string_starts_with(line, "run ")) {
        const char *filename = line + 4;
        int id = loader_spawn(filename);

        if (id >= 0) {
            terminal_write("[INFO] Spawned task ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        }
    }
    else {
        console_warn("Unknown command");
    }

    shell_prompt();
}
