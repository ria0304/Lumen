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


/* Maximum number of commands in a pipeline. */
#define MAX_PIPE_CMDS 8

/* Maximum number of redirections per command. */
#define MAX_REDIRECTIONS 4

/* Redirection types. */
#define REDIR_NONE 0
#define REDIR_IN 1      /* < */
#define REDIR_OUT 2     /* > */
#define REDIR_APPEND 3  /* >> */

/* A single command in a pipeline. */
typedef struct {
    char *argv[32];     /* Command arguments */
    int argc;           /* Number of arguments */
    int in_redir;       /* Input redirection type */
    char *in_file;      /* Input file for redirection */
    int out_redir;      /* Output redirection type */
    char *out_file;     /* Output file for redirection */
} shell_cmd_t;

/* Parse a command line into shell_cmd_t structures for pipeline execution.
 * Returns number of commands in pipeline, or -1 on error. */
static int shell_parse_pipeline(const char *line, shell_cmd_t *cmds, int max_cmds)
{
    int cmd_count = 0;
    const char *cursor = line;
    char token[256];
    int token_len = 0;
    int in_quotes = 0;

    shell_cmd_t *cmd = &cmds[0];
    cmd->argc = 0;
    cmd->in_redir = REDIR_NONE;
    cmd->in_file = 0;
    cmd->out_redir = REDIR_NONE;
    cmd->out_file = 0;

    while (*cursor) {
        char c = *cursor;

        if (c == '"' || c == '\'') {
            if (!in_quotes) {
                in_quotes = c;
            } else if (in_quotes == c) {
                in_quotes = 0;
            }
            cursor++;
            continue;
        }

        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (token_len > 0) {
                token[token_len] = '\0';
                if (cmd->argc < 32) {
                    cmd->argv[cmd->argc++] = kmalloc(token_len + 1);
                    for (int i = 0; i <= token_len; i++)
                        cmd->argv[cmd->argc - 1][i] = token[i];
                }
                token_len = 0;
            }
            cursor++;
            continue;
        }

        if (!in_quotes && c == '|') {
            /* End of current command, start new one in pipeline. */
            if (token_len > 0) {
                token[token_len] = '\0';
                if (cmd->argc < 32) {
                    cmd->argv[cmd->argc++] = kmalloc(token_len + 1);
                    for (int i = 0; i <= token_len; i++)
                        cmd->argv[cmd->argc - 1][i] = token[i];
                }
                token_len = 0;
            }

            if (cmd->argc == 0) {
                console_error("Syntax error: empty command in pipeline");
                return -1;
            }

            cmd_count++;
            if (cmd_count >= MAX_PIPE_CMDS) {
                console_error("Too many commands in pipeline");
                return -1;
            }

            cmd = &cmds[cmd_count];
            cmd->argc = 0;
            cmd->in_redir = REDIR_NONE;
            cmd->in_file = 0;
            cmd->out_redir = REDIR_NONE;
            cmd->out_file = 0;

            cursor++;
            continue;
        }

        if (!in_quotes && c == '>') {
            cursor++;
            if (*cursor == '>') {
                /* >> append */
                cursor++;
                cmd->out_redir = REDIR_APPEND;
            } else {
                /* > */
                cmd->out_redir = REDIR_OUT;
            }

            /* Skip whitespace */
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;

            /* Parse output file name */
            int fn_len = 0;
            while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '|' && fn_len < FS_PATH_MAX - 1) {
                cmd->out_file[fn_len++] = *cursor++;
            }
            cmd->out_file[fn_len] = '\0';
            continue;
        }

        if (!in_quotes && c == '<') {
            cursor++;
            cmd->in_redir = REDIR_IN;

            /* Skip whitespace */
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;

            /* Parse input file name */
            int fn_len = 0;
            while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '|' && fn_len < FS_PATH_MAX - 1) {
                cmd->in_file[fn_len++] = *cursor++;
            }
            cmd->in_file[fn_len] = '\0';
            continue;
        }

        /* Regular character */
        if (token_len < 255) {
            token[token_len++] = c;
        }
        cursor++;
    }

    /* Handle last token */
    if (token_len > 0) {
        token[token_len] = '\0';
        if (cmd->argc < 32) {
            cmd->argv[cmd->argc++] = kmalloc(token_len + 1);
            for (int i = 0; i <= token_len; i++)
                cmd->argv[cmd->argc - 1][i] = token[i];
        }
    }

    if (cmd->argc == 0 && cmd_count == 0) {
        return -1;
    }

    return cmd_count + 1;
}

/* Execute a single command with redirections. */
static int shell_exec_cmd(shell_cmd_t *cmd, int in_fd, int out_fd)
{
    if (cmd->argc == 0)
        return -1;

    int stdin_fd = 0, stdout_fd = 1;

    /* Set up input redirection */
    if (cmd->in_redir == REDIR_IN) {
        fs_handle_t fd;
        if (fs_open(cmd->in_file, FS_ROOT, FS_OPEN_READ, &fd) != FS_OK) {
            console_error("Cannot open input file: ");
            terminal_write(cmd->in_file);
            terminal_putchar('\n');
            return -1;
        }
        stdin_fd = fd;
    } else if (in_fd != 0) {
        stdin_fd = in_fd;
    }

    /* Set up output redirection */
    if (cmd->out_redir == REDIR_OUT) {
        if (fs_write(cmd->out_file, FS_ROOT, "", 0) != FS_OK) {
            console_error("Cannot create output file: ");
            terminal_write(cmd->out_file);
            terminal_putchar('\n');
            return -1;
        }
        fs_handle_t fd;
        if (fs_open(cmd->out_file, FS_ROOT, 0, &fd) != FS_OK) {
            return -1;
        }
        stdout_fd = fd;
    } else if (cmd->out_redir == REDIR_APPEND) {
        fs_handle_t fd;
        if (fs_open(cmd->out_file, FS_ROOT, FS_OPEN_WRITE | FS_OPEN_APPEND, &fd) != FS_OK) {
            console_error("Cannot open output file for append: ");
            terminal_write(cmd->out_file);
            terminal_putchar('\n');
            return -1;
        }
        stdout_fd = fd;
    } else if (out_fd != 1) {
        stdout_fd = out_fd;
    }

    /* Handle built-in commands. */
    if (cmd->argv[0][0] == 'e' && cmd->argv[0][1] == 'c' && cmd->argv[0][2] == 'h' && cmd->argv[0][3] == 'o' && cmd->argv[0][4] == '\0') {
        for (int i = 1; i < cmd->argc; i++) {
            terminal_write(cmd->argv[i]);
            terminal_write(" ");
        }
        terminal_putchar('\n');
        return 0;
    }

    if (cmd->argv[0][0] == 'c' && cmd->argv[0][1] == 'd' && cmd->argv[0][2] == '\0') {
        if (cmd->argc < 2) {
            console_error("cd: missing argument");
            return -1;
        }
        /* TODO: implement cd */
        return 0;
    }

    /* External command - use loader_spawn. */
    int id = loader_spawn(cmd->argv[0]);
    if (id < 0) {
        console_error("Command not found: ");
        terminal_write(cmd->argv[0]);
        terminal_putchar('\n');
        return -1;
    }

    return 0;
}

/* Execute a pipeline of commands. */
static int shell_exec_pipeline(shell_cmd_t *cmds, int cmd_count)
{
    if (cmd_count == 1) {
        return shell_exec_cmd(&cmds[0], 0, 1);
    }

    /* For pipelines, we need to create pipes between commands. */
    int pipe_fds[2];
    int prev_pipe_read = -1;

    for (int i = 0; i < cmd_count; i++) {
        int next_pipe_read = -1, next_pipe_write = -1;

        if (i < cmd_count - 1) {
            int fds[2];
            if (sys_pipe(fds) != 1) {
                console_error("Failed to create pipe");
                return -1;
            }
            next_pipe_read = fds[0];
            next_pipe_write = fds[1];
        }

        int ret = shell_exec_cmd(&cmds[i],
                                 (i == 0) ? 0 : prev_pipe_read,
                                 (i == cmd_count - 1) ? 1 : next_pipe_write);

        /* Close pipe ends we don't need anymore. */
        if (prev_pipe_read != -1) {
            sys_close(prev_pipe_read);
        }
        if (next_pipe_write != -1) {
            sys_close(next_pipe_write);
        }

        if (ret < 0) {
            return -1;
        }

        prev_pipe_read = next_pipe_read;
    }

    return 0;
}



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
    else if (string_starts_with(line, "ln ")) {
        const char *rest = line + 3;
        int symbolic = 0;

        while (*rest == ' ')
            rest++;

        if (string_starts_with(rest, "-s ")) {
            symbolic = 1;
            rest += 3;
        }

        const char *space = 0;
        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                space = p;
                break;
            }
        }

        if (space == 0 || space == rest || space[1] == '\0') {
            console_error("Usage: ln [-s] <target> <link>");
        } else {
            char target[FS_PATH_MAX];
            char linkpath[FS_PATH_MAX];
            uint32_t target_len = (uint32_t)(space - rest);

            if (target_len >= FS_PATH_MAX) {
                console_error("ln: target path too long");
            } else {
                for (uint32_t i = 0; i < target_len; i++)
                    target[i] = rest[i];
                target[target_len] = '\0';

                const char *linkname = space + 1;
                while (*linkname == ' ')
                    linkname++;

                if (*linkname == '\0') {
                    console_error("Usage: ln [-s] <target> <link>");
                } else {
                    for (uint32_t i = 0; i < FS_PATH_MAX - 1 && linkname[i] != '\0'; i++)
                        linkpath[i] = linkname[i];
                    linkpath[FS_PATH_MAX - 1] = '\0';

                    int rc;
                    if (symbolic) {
                        rc = fs_symlink(target, linkpath, FS_ROOT);
                        if (rc == FS_OK)
                            console_info("Symlink created");
                        else
                            console_error("ln: "), terminal_write(fs_strerror(rc)), terminal_putchar('\n');
                    } else {
                        rc = fs_link(target, linkpath, FS_ROOT);
                        if (rc == FS_OK)
                            console_info("Hard link created");
                        else
                            console_error("ln: "), terminal_write(fs_strerror(rc)), terminal_putchar('\n');
                    }
                }
            }
        }
    }
    else if (string_starts_with(line, "readlink ")) {
        const char *linkpath = line + 9;

        while (*linkpath == ' ')
            linkpath++;

        if (*linkpath == '\0') {
            console_error("Usage: readlink <link>");
        } else {
            char target[256];
            int rc = fs_readlink(linkpath, FS_ROOT, target, sizeof(target));

            if (rc == FS_OK) {
                terminal_write(target);
                terminal_putchar('\n');
            } else {
                console_error("readlink: ");
                terminal_write(fs_strerror(rc));
                terminal_putchar('\n');
            }
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
