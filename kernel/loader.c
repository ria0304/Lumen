#include <stdint.h>
#include "loader.h"
#include "fs.h"
#include "task.h"
#include "paging.h"
#include "heap.h"
#include "console.h"

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
