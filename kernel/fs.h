#ifndef FS_H
#define FS_H

#include <stdint.h>

/*
 * LumenFS: a deliberately simple flat filesystem for the second
 * ATA disk (build/disk.img). Design, spelled out because none of
 * it is standard:
 *
 *   LBA 0          Superblock (magic, format check, next free LBA
 *                   for new file data).
 *   LBA 1..32       Fixed directory: FS_MAX_FILES entries, no
 *                   subdirectories. Each entry is name + size +
 *                   start LBA + used flag.
 *   LBA 33 onward   File data, allocated by bumping the
 *                   superblock's next-free-LBA forward -- like
 *                   kernel/heap.c's kmalloc, space is never
 *                   reclaimed when a file is deleted, only the
 *                   directory entry is freed. Fine for a hobby OS
 *                   at this stage; a real free-list is future work.
 *
 * There are no directories, no permissions, and a deleted file's
 * data blocks are never reused -- fs_delete() only frees the
 * directory slot. FS_MAX_FILE_SECTORS caps a single file at 8 KiB
 * so the loader can always read one whole file into one heap
 * buffer without needing to know its size up front.
 */

#define FS_MAGIC              0x4C554D46U /* "LUMF" */
#define FS_MAX_FILES          64
#define FS_NAME_LEN           28
#define FS_DIR_START_LBA      1U
#define FS_DIR_SECTORS        5U /* 64 * 40 bytes = 2560 bytes = 5 sectors */
#define FS_DATA_START_LBA     (FS_DIR_START_LBA + FS_DIR_SECTORS)
#define FS_MAX_FILE_SECTORS   16U /* 8 KiB per file, hard cap */
#define FS_DISK_SECTORS       8192U /* 4 MiB QEMU disk image */

typedef struct {
    char name[FS_NAME_LEN];
    uint32_t size_bytes;
    uint32_t start_lba;
    uint8_t used;
} __attribute__((packed)) fs_entry_t;

/*
 * Mounts LumenFS from disk. If the superblock's magic doesn't
 * match, the filesystem is treated as unformatted -- every call
 * below fails until fs_format() is run. Safe to call whether or
 * not ata_is_ready(); if the disk isn't ready, this just leaves
 * the filesystem unmounted.
 */
void fs_init(void);

int fs_is_mounted(void);

/* Wipes the directory and resets the free-space pointer, then
 * writes a fresh superblock and directory to disk. */
int fs_format(void);

/* Creates (or truncates, if it already exists) a file of exactly
 * 'size' bytes and writes 'data' into it. Returns 0 on success. */
int fs_write(const char *name, const void *data, uint32_t size);

/* Reads up to 'buffer_size' bytes of 'name' into 'buffer'. Returns
 * the number of bytes actually read, or -1 if the file doesn't
 * exist. The returned count can be less than the file's real size
 * if it doesn't fit in 'buffer_size'. */
int fs_read(const char *name, void *buffer, uint32_t buffer_size);

int fs_delete(const char *name);

/* Returns the entry for 'name', or 0 if it doesn't exist. */
const fs_entry_t *fs_stat(const char *name);

/* Prints every file's name and size via terminal_write(). */
void fs_list(void);

uint32_t fs_file_count(void);

/* End-to-end filesystem/storage verification. */
int fs_self_test(void);

#endif
