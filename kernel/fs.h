#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fs_format.h"

/*
 * LumenFS v2.
 *
 * A persistent block-allocated filesystem with directories, inodes,
 * and per-inode ownership and permission bits. See fs_format.h for
 * the on-disk layout, which is shared with the host-side mkfs.
 *
 * A LumenFS v1 image is rejected at mount rather than reinterpreted;
 * the two formats have nothing in common beyond the first sector.
 *
 * Error codes are negative and distinct, so callers can tell "no such
 * file" from "permission denied" and report it as such:
 *
 *   FS_OK           0   success
 *   FS_ERR          -1  unspecified failure
 *   FS_ENOENT       -2  no such file or directory
 *   FS_EEXIST       -3  already exists
 *   FS_ENOTDIR      -4  not a directory
 *   FS_EISDIR       -5  is a directory
 *   FS_EACCES       -6  permission denied
 *   FS_ENOSPC       -7  no free blocks
 *   FS_EINVAL       -8  bad argument
 *   FS_ENOTEMPTY    -9  directory not empty
 *   FS_ENAMETOOLONG -10 name too long
 *   FS_ELOOP       -11 too many levels of symlink / path depth
 *   FS_EIO         -12 disk reported an error
 *   FS_EBADF       -13 no such inode, or it is not a file
 *   FS_ENOSYS      -14 operation not supported
 *   FS_ENOMEM      -15 out of kernel memory
 */
#define FS_OK           0
#define FS_ERR          -1
#define FS_ENOENT       -2
#define FS_EEXIST       -3
#define FS_ENOTDIR      -4
#define FS_EISDIR       -5
#define FS_EACCES       -6
#define FS_ENOSPC       -7
#define FS_EINVAL       -8
#define FS_ENOTEMPTY    -9
#define FS_ENAMETOOLONG -10
#define FS_ELOOP        -11
#define FS_EIO          -12
#define FS_EBADF        -13
#define FS_ENOSYS       -14
#define FS_ENOMEM       -15

/* Maximum path components walked before giving up. */
#define FS_MAX_DEPTH 16

/* Handle for an open file, used by the read/write/lseek calls. */
typedef struct {
    uint32_t inode;       /* inode number, or FS_BLOCK_NONE if closed */
    uint32_t offset;      /* byte offset for sequential reads */
    uint32_t flags;
} fs_handle_t;

/* SEEK_* values for fs_handle_seek(). */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Open flags. */
#define FS_OPEN_READ    0x01
#define FS_OPEN_WRITE   0x02
#define FS_OPEN_CREATE  0x04
#define FS_OPEN_TRUNC   0x08
#define FS_OPEN_APPEND  0x10

/* The identity a filesystem operation is performed as. Every
 * permission check uses one of these, so there is no implicit "the
 * kernel" caller that bypasses the checks. */
typedef struct {
    uint16_t uid;
    uint16_t gid;
} fs_cred_t;

/* The superuser. Bypasses the permission bits, exactly as root does
 * on a real system. Used by the shell. */
extern const fs_cred_t FS_ROOT;

/* An unprivileged identity, for testing the permission checks. */
extern const fs_cred_t FS_NOBODY;

void fs_init(void);

/* True when a valid v2 filesystem is mounted and usable. */
int fs_is_mounted(void);

/* Wipe the disk and lay down a fresh empty v2 filesystem with a
 * root directory owned by uid/gid. Returns FS_OK. */
int fs_format(uint16_t uid, uint16_t gid);

/*
 * Fill 'out' with the geometry a format of 'total_sectors' would use,
 * without touching the disk. Exposed so tools/mkfs.c and the kernel
 * agree on the layout by construction rather than by convention.
 */
int fs_plan_layout(uint32_t total_sectors, fs_superblock_t *out);

/* Mount a v2 filesystem from disk. Returns FS_EINVAL if the image is
 * not a v2 filesystem (including a v1 image, which is rejected rather
 * than reinterpreted). */
int fs_mount(void);

/* Read the superblock of whatever is on the disk without mounting.
 * Returns FS_OK and fills *out, or an error. */
int fs_read_superblock(fs_superblock_t *out);

/* Human-readable form of an FS_ERR* code. */
const char *fs_strerror(int code);

/* Total and free capacity, in bytes. */
uint64_t fs_total_bytes(void);
uint64_t fs_free_bytes(void);

/* ---- Metadata ------------------------------------------------------ */

int fs_stat(const char *path, fs_cred_t cred, fs_inode_t *out);

/*
 * Read an inode's metadata by number, for callers that already have
 * one (directory listings) and need not resolve a path to see it.
 */
int fs_stat_by_inode(uint32_t ino, fs_inode_t *out);

/* Change a file's permission bits. */
int fs_chmod(const char *path, fs_cred_t cred, uint8_t mode);

/* Change a file's owner. Requires FS_PERM_OWNER_* on the file and,
 * when changing the owner, superuser privilege. */
int fs_chown(const char *path, fs_cred_t cred, uint16_t uid, uint16_t gid);

/* Rename a file or directory within its parent. */
int fs_rename(const char *from, const char *to, fs_cred_t cred);

/* ---- Whole-file convenience calls ---------------------------------- */

/* Create or overwrite a file in one shot. Used by the shell's
 * `write` and by the filesystem self-test. */
int fs_write(const char *path, fs_cred_t cred,
             const void *data, uint32_t size);

/* Read a whole file. *out is heap-allocated and must be kfree'd by
 * the caller; *out_size receives the length. */
int fs_read(const char *path, fs_cred_t cred,
            void **out, uint32_t *out_size);

/* Remove a file, or an empty directory. */
int fs_delete(const char *path, fs_cred_t cred);

/* Truncate to zero length, keeping the inode. */
int fs_truncate(const char *path, fs_cred_t cred);

/* Create a symbolic link. */
int fs_symlink(const char *target, const char *linkpath, fs_cred_t cred);

/* Read the target of a symbolic link. */
int fs_readlink(const char *linkpath, fs_cred_t cred, char *buf, uint32_t buf_size);

/* Create a hard link to an existing file. */
int fs_link(const char *existing_path, const char *new_path, fs_cred_t cred);

/* ---- Directories --------------------------------------------------- */

int fs_mkdir(const char *path, fs_cred_t cred, uint8_t mode);
int fs_rmdir(const char *path, fs_cred_t cred);

/* Create a file inode without writing contents. */
int fs_create_file(const char *path, fs_cred_t cred, uint8_t mode,
                   uint32_t *out_inode);

/* Resolve a path, optionally requiring it to be a directory. */
int fs_resolve_dir(const char *path, fs_cred_t cred, uint32_t *out_inode);

/*
 * Resolve a path to its inode number. want_type of 0 accepts any
 * type, FS_TYPE_DIR requires a directory, FS_TYPE_FILE a regular
 * file. Returns ENOTDIR when the path names a directory where a file
 * was required, and EBADF the other way round.
 */
int fs_lookup(const char *path, fs_cred_t cred, uint8_t want_type,
              uint32_t *out_inode);

/* Number of entries in a directory. */
int fs_count_entries(const char *path, fs_cred_t cred, uint32_t *out_count);

/*
 * Fetch directory entry 'index' of 'path'. On success *out_inode
 * receives the inode number and *out_name a pointer to a
 * NUL-terminated name, both owned by an internal buffer that is
 * overwritten by the next call.
 */
int fs_list(const char *path, fs_cred_t cred, uint32_t index,
            uint32_t *out_inode, const char **out_name, uint8_t *out_type);

/* ---- Handles ------------------------------------------------------- */

int fs_open(const char *path, fs_cred_t cred, uint32_t flags,
            fs_handle_t *out);
int fs_close(fs_handle_t *handle);

int fs_handle_read(fs_handle_t *handle, void *buffer, uint32_t size,
                   uint32_t *out_read);
int fs_handle_write(fs_handle_t *handle, const void *buffer, uint32_t size,
                    uint32_t *out_written);
int fs_handle_seek(fs_handle_t *handle, uint32_t offset, uint32_t whence);
int fs_handle_size(fs_handle_t *handle, uint32_t *out_size);

/* ---- Self-test ----------------------------------------------------- */

/*
 * Exercises allocation, nested directories, persistence across a
 * remount, permission enforcement, and freed-block reuse. Returns 0
 * on success, non-zero on failure, matching the *_run_self_test
 * convention used elsewhere in the kernel.
 *
 * Destructive: it creates and removes its own files, and rewrites the
 * working directory context while doing so.
 */
int fs_self_test(void);

#endif
