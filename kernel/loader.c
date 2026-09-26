#include <stdint.h>
#include "loader.h"
#include "fs.h"
#include "task.h"
#include "paging.h"
#include "heap.h"
#include "console.h"

int loader_spawn(const char *filename)
{
    const fs_entry_t *entry = fs_stat(filename);

    if (entry == 0) {
        console_error("Loader: file not found");
        return -1;
    }

    if (entry->size_bytes == 0 || entry->size_bytes > PAGE_SIZE) {
        console_error("Loader: file empty or larger than one page");
        return -1;
    }

    uint8_t *buffer = (uint8_t *)kmalloc(PAGE_SIZE);

    if (buffer == 0) {
        console_error("Loader: out of memory");
        return -1;
    }

    int read = fs_read(filename, buffer, PAGE_SIZE);

    if (read <= 0) {
        console_error("Loader: read failed");
        kfree(buffer);
        return -1;
    }

    int id = task_create_user_program(buffer, (uint32_t)read);

    kfree(buffer);

    if (id < 0) {
        console_error("Loader: task creation failed");
        return -1;
    }

    return id;
}
