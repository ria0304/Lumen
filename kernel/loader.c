#include <stdint.h>
#include "loader.h"
#include "fs.h"
#include "task.h"
#include "paging.h"
#include "heap.h"
#include "console.h"
#include "elf.h"

/*
 * Load a flat binary from LumenFS and start it as a Ring 3 task.
 *
 * The file is read whole through the v2 filesystem, which heap-allocates
 * the exact size rather than assuming a page. The PAGE_SIZE cap remains
 * because a Ring 3 task currently has exactly one code page mapped at
 * USER_CODE_ADDRESS; lifting that is part of the ABI work, and a larger
 * file would silently overwrite unmapped memory otherwise.
 */
int loader_spawn(const char *filename)
{
    if (!fs_is_mounted()) {
        console_error("Loader: no filesystem mounted");
        return -1;
    }

    /*
     * Programs are loaded on behalf of the shell, which runs as root.
     * Loading still goes through the permission checks rather than
     * bypassing them, so a file a normal user could not read is
     * refused here too.
     */
    fs_inode_t meta;
    int rc = fs_stat(filename, FS_ROOT, &meta);

    if (rc != FS_OK) {
        console_error("Loader: cannot stat file: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return -1;
    }

    if (meta.type == FS_TYPE_DIR) {
        console_error("Loader: is a directory, not a program");
        return -1;
    }

    if (meta.size == 0) {
        console_error("Loader: file is empty");
        return -1;
    }

    if (meta.size > PAGE_SIZE) {
        console_error("Loader: larger than one page (");
        terminal_write_u32(meta.size);
        terminal_write(" bytes); ABI work pending\n");
        return -1;
    }

    void *buffer = 0;
    uint32_t size = 0;

    rc = fs_read(filename, FS_ROOT, &buffer, &size);

    if (rc != FS_OK) {
        console_error("Loader: read failed: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return -1;
    }

    if (size == 0) {
        console_error("Loader: read returned no data");
        kfree(buffer);
        return -1;
    }

    int id = task_create_user_program(buffer, size);

    kfree(buffer);

    if (id < 0) {
        console_error("Loader: task creation failed");
        return -1;
    }

    return id;
}

/* Load an ELF executable from the filesystem and start it as a Ring 3 task. */
int loader_spawn_elf(const char *filename)
{
    if (!fs_is_mounted()) {
        console_error("Loader: no filesystem mounted");
        return -1;
    }

    fs_inode_t meta;
    int rc = fs_stat(filename, FS_ROOT, &meta);

    if (rc != FS_OK) {
        console_error("Loader: cannot stat file: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return -1;
    }

    if (meta.type == FS_TYPE_DIR) {
        console_error("Loader: is a directory, not a program");
        return -1;
    }

    if (meta.size == 0) {
        console_error("Loader: file is empty");
        return -1;
    }

    if (meta.size > 1024 * 1024) {
        console_error("Loader: file too large (");
        terminal_write_u32(meta.size);
        terminal_write(" bytes); max 1 MiB\n");
        return -1;
    }

    /* Read the entire file into memory. */
    void *buffer = 0;
    uint32_t size = 0;

    rc = fs_read(filename, FS_ROOT, &buffer, &size);

    if (rc != FS_OK) {
        console_error("Loader: read failed: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return -1;
    }

    if (size == 0) {
        console_error("Loader: read returned no data");
        kfree(buffer);
        return -1;
    }

    /* Verify ELF magic. */
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)buffer;

    if (size < sizeof(Elf32_Ehdr) ||
        ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        console_error("Loader: not a valid ELF executable");
        kfree(buffer);
        return -1;
    }

    /* Check architecture and type. */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_type != ET_EXEC ||
        ehdr->e_machine != EM_386) {
        console_error("Loader: unsupported ELF type/arch");
        kfree(buffer);
        return -1;
    }

    /* Create the task with a fresh address space. */
    int task_id = task_create_user_elf(ehdr, buffer, size);

    kfree(buffer);

    if (task_id < 0) {
        console_error("Loader: task creation failed");
        return -1;
    }

    return task_id;
}
