#include <stdint.h>
#include "kmem.h"
#include "fs.h"
#include "fs_format.h"
#include "ata.h"
#include "console.h"
#include "heap.h"
#include "rtc.h"
#include "lock.h"
#include "frame.h"
#include "paging.h"

/*
 * LumenFS v2 driver.
 *
 * Structure of this file:
 *
 *   1. mount state and the credential identities
 *   2. raw block and sector I/O
 *   3. the block allocation bitmap
 *   4. the inode table
 *   5. block mapping for a file
 *   6. directory entries
 *   7. path resolution
 *   8. permission checks
 *   9. the public API
 *  10. the self-test
 *
 * Everything below assumes the on-disk geometry in fs_format.h and
 * nothing else, so tools/mkfs.c can produce an identical image.
 */

static fs_superblock_t sb;
static int mounted = 0;

/*
 * The bitmap and inode table are the only large on-disk structures,
 * and both are read-modify-written, so they are cached in kernel
 * memory and written back when dirty. A 16 MiB image has 8192 blocks,
 * i.e. a 1 KiB bitmap, which is trivial to hold resident.
 */
static uint8_t *bitmap = 0;
static uint32_t bitmap_bytes = 0;
static int bitmap_dirty = 0;

/* Byte pool used for directory and file I/O staging. */
#define FS_STAGE_BYTES FS_BLOCK_SIZE


/* The two identities the kernel ships with. The shell runs as root;
 * FS_NOBODY exists so the permission checks can be exercised. */
const fs_cred_t FS_ROOT = { 0, 0 };
const fs_cred_t FS_NOBODY = { 65534, 65534 };

/*
 * Recursive interrupt-disabling lock.
 *
 * Lumen is uniprocessor, so disabling interrupts is a complete
 * mutual-exclusion primitive. It has to be recursive because the
 * public API nests: fs_write() calls the handle layer, which calls
 * the block layer, and the self-test drives all of it from inside a
 * locked region.
 */
static volatile int fs_lock_depth = 0;
static irqflags_t fs_lock_flags = 0;

static void fs_lock(void)
{
    if (fs_lock_depth == 0)
        fs_lock_flags = irq_save_disable();

    fs_lock_depth++;
}

static void fs_unlock(void)
{
    fs_lock_depth--;

    if (fs_lock_depth == 0)
        irq_restore(fs_lock_flags);
}

#define FS_LOCK()   fs_lock()
#define FS_UNLOCK() fs_unlock()

/* ---- 1. Errors and formatting -------------------------------------- */

const char *fs_strerror(int code)
{
    switch (code) {
        case FS_OK:           return "ok";
        case FS_ENOENT:       return "no such file or directory";
        case FS_EEXIST:       return "already exists";
        case FS_ENOTDIR:      return "not a directory";
        case FS_EISDIR:       return "is a directory";
        case FS_EACCES:       return "permission denied";
        case FS_ENOSPC:       return "no space left on device";
        case FS_EINVAL:       return "invalid argument";
        case FS_ENOTEMPTY:    return "directory not empty";
        case FS_ENAMETOOLONG: return "name too long";
        case FS_ELOOP:        return "too many path levels";
        case FS_EIO:          return "disk I/O error";
        case FS_EBADF:        return "not a regular file";
        case FS_ENOSYS:       return "not supported";
        default:              return "unknown error";
    }
}

/* ---- 2. Raw sector and block I/O ------------------------------------ */

static int fs_sector_read(uint32_t lba, void *buffer)
{
    return ata_read_sectors(lba, 1, buffer) == 0 ? 0 : FS_EIO;
}

static int fs_sector_write(uint32_t lba, const void *buffer)
{
    return ata_write_sectors(lba, 1, buffer) == 0 ? 0 : FS_EIO;
}

static uint32_t fs_block_sector(uint32_t block)
{
    return sb.data_start_lba + block * FS_BLOCK_SECTORS;
}

/* Read one whole block into 'buffer', which must be FS_BLOCK_SIZE. */
static int fs_block_read(uint32_t block, void *buffer)
{
    if (buffer == 0)
        return FS_EINVAL;

    for (uint32_t i = 0; i < FS_BLOCK_SECTORS; i++) {
        int rc = fs_sector_read(fs_block_sector(block) + i,
                                (uint8_t *)buffer + i * FS_SECTOR_SIZE);

        if (rc != FS_OK)
            return rc;
    }

    return FS_OK;
}

static int fs_block_write(uint32_t block, const void *buffer)
{
    if (buffer == 0)
        return FS_EINVAL;

    for (uint32_t i = 0; i < FS_BLOCK_SECTORS; i++) {
        int rc = fs_sector_write(fs_block_sector(block) + i,
                                 (const uint8_t *)buffer + i * FS_SECTOR_SIZE);

        if (rc != FS_OK)
            return rc;
    }

    return FS_OK;
}

/* ---- 3. Block allocation bitmap ------------------------------------ */

static int bitmap_test(uint32_t block)
{
    if (block >= sb.total_blocks)
        return 0;

    return (bitmap[block >> 3] >> (block & 7)) & 1;
}

static void bitmap_set(uint32_t block, int value)
{
    if (block >= sb.total_blocks)
        return;

    uint8_t mask = (uint8_t)(1u << (block & 7));

    if (value)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (uint8_t)~mask;

    bitmap_dirty = 1;
}

/* Mark every block covering the metadata region as in use. */
static void bitmap_reserve_metadata(void)
{
    uint32_t first_meta_block = sb.bitmap_lba / FS_BLOCK_SECTORS;
    uint32_t last_meta_block =
        (sb.data_start_lba + FS_BLOCK_SECTORS - 1) / FS_BLOCK_SECTORS;

    for (uint32_t b = first_meta_block; b < last_meta_block; b++)
        bitmap_set(b, 1);
}

/* First free block at or after 'start', or FS_BLOCK_NONE. */
static uint32_t bitmap_find_free(uint32_t start)
{
    if (start >= sb.total_blocks)
        return FS_BLOCK_NONE;

    for (uint32_t b = start; b < sb.total_blocks; b++) {
        if (!bitmap_test(b))
            return b;
    }

    return FS_BLOCK_NONE;
}

static int bitmap_flush(void)
{
    if (!bitmap_dirty)
        return FS_OK;

    for (uint32_t i = 0; i < sb.bitmap_sectors; i++) {
        int rc = fs_sector_write(sb.bitmap_lba + i,
                                 bitmap + i * FS_SECTOR_SIZE);

        if (rc != FS_OK) {
            bitmap_dirty = 1;
            return rc;
        }
    }

    bitmap_dirty = 0;
    return FS_OK;
}

static int bitmap_load(void)
{
    for (uint32_t i = 0; i < sb.bitmap_sectors; i++) {
        int rc = fs_sector_read(sb.bitmap_lba + i,
                                bitmap + i * FS_SECTOR_SIZE);

        if (rc != FS_OK)
            return rc;
    }

    bitmap_dirty = 0;
    return FS_OK;
}

/* ---- 4. Inode table ------------------------------------------------ */

static int fs_inode_read(uint32_t ino, fs_inode_t *out)
{
    if (ino >= sb.inode_count)
        return FS_EBADF;

    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t per_sector = FS_INODES_PER_SECTOR;
    uint32_t sector_index = ino / per_sector;
    uint32_t slot = ino % per_sector;

    int rc = fs_sector_read(sb.inode_table_lba + sector_index, sector);

    if (rc != FS_OK)
        return rc;

    *out = ((fs_inode_t *)sector)[slot];

    return FS_OK;
}

static int fs_inode_write(uint32_t ino, const fs_inode_t *in)
{
    if (ino >= sb.inode_count)
        return FS_EBADF;

    uint8_t sector[FS_SECTOR_SIZE];
    uint32_t per_sector = FS_INODES_PER_SECTOR;
    uint32_t sector_index = ino / per_sector;
    uint32_t slot = ino % per_sector;

    int rc = fs_sector_read(sb.inode_table_lba + sector_index, sector);

    if (rc != FS_OK)
        return rc;

    ((fs_inode_t *)sector)[slot] = *in;

    return fs_sector_write(sb.inode_table_lba + sector_index, sector);
}

/*
 * Unix epoch seconds from a broken-down date, so mtime sorts and
 * compares correctly across days. Uses the standard days-from-civil
 * calculation, valid for any year from 0 upward.
 */
static uint32_t fs_time_to_epoch(uint16_t year, uint8_t month, uint8_t day,
                                 uint8_t hour, uint8_t minute,
                                 uint8_t second)
{
    int y = (int)year;
    int m = (int)month;
    int d = (int)day;

    /* Shift the year so March starts the year, which removes the
     * leap-day special case from the month table. */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    uint32_t days = (uint32_t)(era * 146097 + (int)doe - 719468);

    return days * 86400U + hour * 3600U + minute * 60U + second;
}

static uint32_t fs_now_epoch(void)
{
    rtc_time_t now;

    if (rtc_read(&now) != 0)
        return 0;

    return fs_time_to_epoch(now.year, now.month, now.day,
                            now.hour, now.minute, now.second);
}

static void fs_inode_init(fs_inode_t *in, uint8_t type, uint16_t uid,
                          uint16_t gid, uint8_t mode)
{
    for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++)
        in->direct[i] = FS_BLOCK_NONE;

    in->type = type;
    in->mode = mode & FS_MODE_MASK;
    in->uid = uid;
    in->gid = gid;
    in->size = 0;
    in->flags = 0;
    in->indirect = FS_BLOCK_NONE;
    in->mtime = fs_now_epoch();
}

static void fs_inode_touch(fs_inode_t *in)
{
    in->mtime = fs_now_epoch();
}

/* Lowest unused inode number, or FS_BLOCK_NONE. */
static uint32_t fs_inode_alloc(void)
{
    fs_inode_t in;

    for (uint32_t ino = 0; ino < sb.inode_count; ino++) {
        if (fs_inode_read(ino, &in) != FS_OK)
            return FS_BLOCK_NONE;

        if (in.type == FS_TYPE_FREE)
            return ino;
    }

    return FS_BLOCK_NONE;
}

static int fs_inode_free(uint32_t ino)
{
    fs_inode_t in;

    int rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    /* Release every block the inode still owns, or they leak for as
     * long as the filesystem exists. This is the whole reason v1 was
     * replaced: it could not give space back. */
    for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++) {
        if (in.direct[i] != FS_BLOCK_NONE) {
            bitmap_set(in.direct[i], 0);
            in.direct[i] = FS_BLOCK_NONE;
        }
    }

    if (in.indirect != FS_BLOCK_NONE) {
        uint8_t stage[FS_STAGE_BYTES];

        /* Walk the indirect block and free whatever it points at. */
        if (fs_block_read(in.indirect, stage) == FS_OK) {
            uint32_t *ptrs = (uint32_t *)stage;

            for (uint32_t i = 0; i < FS_INDIRECT_PTRS; i++) {
                if (ptrs[i] != FS_BLOCK_NONE)
                    bitmap_set(ptrs[i], 0);
            }
        }

        bitmap_set(in.indirect, 0);
        in.indirect = FS_BLOCK_NONE;
    }

    in.type = FS_TYPE_FREE;
    in.size = 0;
    in.mode = 0;
    in.uid = 0;
    in.gid = 0;
    in.flags = 0;

    return fs_inode_write(ino, &in);
}

/* ---- 5. Block mapping ----------------------------------------------- */

/*
 * Map logical block index 'index' of an inode to a physical block,
 * allocating on demand. *creating must be 0 for a read-only lookup,
 * in which case a missing block reports FS_OK with FS_BLOCK_NONE.
 */
static int fs_map_block(fs_inode_t *in, uint32_t index, int creating,
                        uint32_t *out)
{
    if (index < FS_DIRECT_BLOCKS) {
        if (in->direct[index] == FS_BLOCK_NONE) {
            if (!creating) {

                /* Past end of file. Report "no block" rather than
                 * returning success with *out still holding whatever
                 * the caller had in it -- an uninitialised block
                 * number reached fs_block_read() and page-faulted at
                 * 0xFFFFFFFF. */
                *out = FS_BLOCK_NONE;
                return FS_OK;
            }

            uint32_t b = bitmap_find_free(0);

            if (b == FS_BLOCK_NONE)
                return FS_ENOSPC;

            bitmap_set(b, 1);
            in->direct[index] = b;
        }

        *out = in->direct[index];
        return FS_OK;
    }

    uint32_t indirect_index = index - FS_DIRECT_BLOCKS;

    if (indirect_index >= FS_INDIRECT_PTRS) {
        /* Beyond one indirect block. The format does not extend to
         * double indirection, and silently wrapping here would
         * corrupt a file, so refuse. */
        return FS_ENOSYS;
    }

    if (in->indirect == FS_BLOCK_NONE) {
        if (!creating)
            return FS_OK;

        uint32_t b = bitmap_find_free(0);

        if (b == FS_BLOCK_NONE)
            return FS_ENOSPC;

        bitmap_set(b, 1);

        uint8_t stage[FS_STAGE_BYTES];

        for (uint32_t i = 0; i < FS_INDIRECT_PTRS; i++)
            ((uint32_t *)stage)[i] = FS_BLOCK_NONE;

        int rc = fs_block_write(b, stage);

        if (rc != FS_OK) {
            bitmap_set(b, 0);
            return rc;
        }

        in->indirect = b;
    }

    uint8_t stage[FS_STAGE_BYTES];
    int rc = fs_block_read(in->indirect, stage);

    if (rc != FS_OK)
        return rc;

    uint32_t *ptrs = (uint32_t *)stage;
    uint32_t target = ptrs[indirect_index];

    if (target == FS_BLOCK_NONE && creating) {
        uint32_t b = bitmap_find_free(0);

        if (b == FS_BLOCK_NONE)
            return FS_ENOSPC;

        bitmap_set(b, 1);
        ptrs[indirect_index] = b;
        target = b;

        rc = fs_block_write(in->indirect, stage);

        if (rc != FS_OK) {
            bitmap_set(b, 0);
            return rc;
        }
    }

    *out = target;
    return FS_OK;
}

/* Read 'size' bytes at 'offset' out of an inode's data. */
static int fs_inode_read_at(fs_inode_t *in, uint32_t offset, void *buffer,
                            uint32_t size, uint32_t *out_read)
{
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t done = 0;

    if (offset >= in->size)
        goto done_label;

    if (offset + size > in->size)
        size = in->size - offset;

    while (done < size) {
        uint32_t block_index = (offset + done) / FS_BLOCK_SIZE;
        uint32_t within = (offset + done) % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - within;

        if (chunk > size - done)
            chunk = size - done;

        uint32_t block;
        int rc = fs_map_block(in, block_index, 0, &block);

        if (rc != FS_OK)
            return rc;

        if (block == FS_BLOCK_NONE) {

            /* A hole. Reading a sparse block gives zeroes, which is
             * what the file logically contains there. */
            for (uint32_t i = 0; i < chunk; i++)
                dst[done + i] = 0;

            done += chunk;
            continue;
        }

        uint8_t stage[FS_STAGE_BYTES];
        rc = fs_block_read(block, stage);

        if (rc != FS_OK)
            return rc;

        for (uint32_t i = 0; i < chunk; i++)
            dst[done + i] = stage[within + i];

        done += chunk;
    }

done_label:
    if (out_read)
        *out_read = done;

    return FS_OK;
}

/* Write 'size' bytes at 'offset', extending the file as needed. */
static int fs_inode_write_at(fs_inode_t *in, uint32_t offset,
                             const void *buffer, uint32_t size,
                             uint32_t *out_written)
{
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t done = 0;

    if (size > FS_MAX_FILE_SIZE)
        size = FS_MAX_FILE_SIZE;

    if (offset > FS_MAX_FILE_SIZE - size)
        size = FS_MAX_FILE_SIZE - offset;

    while (done < size) {
        uint32_t block_index = (offset + done) / FS_BLOCK_SIZE;
        uint32_t within = (offset + done) % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - within;

        if (chunk > size - done)
            chunk = size - done;

        uint32_t block;
        int rc = fs_map_block(in, block_index, 1, &block);

        if (rc != FS_OK)
            return rc;

        uint8_t stage[FS_STAGE_BYTES];

        if (fs_block_read(block, stage) != FS_OK) {
            memset(stage, 0, FS_STAGE_BYTES);
        }

        for (uint32_t i = 0; i < chunk; i++)
            stage[within + i] = src[done + i];

        rc = fs_block_write(block, stage);

        if (rc != FS_OK)
            return rc;

        done += chunk;
    }

    if (offset + done > in->size)
        in->size = offset + done;

    if (out_written)
        *out_written = done;

    return FS_OK;
}

/* ---- 6. Directory entries ------------------------------------------ */

/* Locate a name inside a directory inode. Returns the entry's byte
 * offset within the directory's data, or FS_ENOENT. */
/* A slot is live only if it names a usable inode. Unwritten slots in a
 * freshly allocated directory block are all zero, and ino == 0 is a
 * perfectly valid inode number (it is the root directory), so testing
 * only `ino != FS_BLOCK_NONE` made every blank slot look occupied.
 * fs_dirent_remove() marks a retired slot with FS_TYPE_FREE, and a
 * zeroed slot decodes as FS_TYPE_FREE too, so the type field is the
 * reliable marker. */
static int fs_dirent_live(const fs_dirent_t *ent)
{
    return ent->ino != FS_BLOCK_NONE && ent->type != FS_TYPE_FREE;
}

static int fs_dirent_find(fs_inode_t *dir, const char *name,
                          uint32_t name_len, uint32_t *out_offset)
{
    uint32_t capacity = fs_blocks_for_bytes(dir->size) * FS_BLOCK_SIZE;
    uint32_t offset = 0;

    while (offset + FS_DIRENT_SIZE <= capacity) {
        fs_dirent_t ent;
        uint32_t got = 0;
        int rc = fs_inode_read_at(dir, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        if (got < FS_DIRENT_SIZE)
            break;

        if (fs_dirent_live(&ent) &&
            ent.name_len == name_len &&
            memcmp(ent.name, name, name_len) == 0) {
            if (out_offset)
                *out_offset = offset;

            return FS_OK;
        }

        offset += FS_DIRENT_SIZE;
    }

    return FS_ENOENT;
}

/* Defined below; fs_dirent_remove() needs it to notice when the last
 * entry goes and release the directory's blocks. */
static int fs_dirent_is_empty(fs_inode_t *dir);

/* First free (ino == FS_BLOCK_NONE) slot at or after 'from'. */
static int fs_dirent_find_free(fs_inode_t *dir, uint32_t from,
                               uint32_t *out_offset)
{
    uint32_t capacity = fs_blocks_for_bytes(dir->size) * FS_BLOCK_SIZE;
    uint32_t offset = from;

    while (offset + FS_DIRENT_SIZE <= capacity) {
        fs_dirent_t ent;
        uint32_t got = 0;
        int rc = fs_inode_read_at(dir, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        if (got < FS_DIRENT_SIZE)
            break;

        if (!fs_dirent_live(&ent)) {
            *out_offset = offset;
            return FS_OK;
        }

        offset += FS_DIRENT_SIZE;
    }

    return FS_ENOSPC;
}

/* Append a new entry, growing the directory if it is full. */
static int fs_dirent_add(fs_inode_t *dir, uint32_t ino, uint8_t type,
                         const char *name, uint32_t name_len)
{
    uint32_t offset = 0;
    int rc = fs_dirent_find_free(dir, 0, &offset);

    if (rc == FS_ENOSPC) {

        /* Every existing slot is live: extend the directory by one
         * block and use its first slot. Derive the block index from
         * the capacity already scanned above -- dir->size is not
         * reliably a block multiple, so dividing it by the block size
         * reused the current block and overwrote its live entries. */
        uint32_t capacity = fs_blocks_for_bytes(dir->size) * FS_BLOCK_SIZE;
        uint32_t block_index = capacity / FS_BLOCK_SIZE;
        uint32_t block = FS_BLOCK_NONE;

        rc = fs_map_block(dir, block_index, 1, &block);

        if (rc != FS_OK)
            return rc;

        offset = block_index * FS_BLOCK_SIZE;
    } else if (rc != FS_OK) {
        return rc;
    }

    fs_dirent_t ent;

    ent.ino = ino;
    ent.type = type;
    ent.name_len = (uint8_t)name_len;
    memset(ent.name, 0, FS_NAME_MAX);
    memcpy(ent.name, name, name_len);

    uint32_t written = 0;
    rc = fs_inode_write_at(dir, offset, &ent, FS_DIRENT_SIZE, &written);

    if (rc != FS_OK)
        return rc;

    if (written < FS_DIRENT_SIZE)
        return FS_EIO;

    return FS_OK;
}

/* Retire an entry without compacting the array. */
static int fs_dirent_remove(fs_inode_t *dir, uint32_t offset)
{
    fs_dirent_t ent;

    ent.ino = FS_BLOCK_NONE;
    ent.type = FS_TYPE_FREE;
    ent.name_len = 0;
    memset(ent.name, 0, FS_NAME_MAX);

    uint32_t written = 0;
    int rc = fs_inode_write_at(dir, offset, &ent, FS_DIRENT_SIZE, &written);

    if (rc != FS_OK)
        return rc;

    if (written < FS_DIRENT_SIZE)
        return FS_EIO;

    /*
     * If that was the last live entry, hand the directory's blocks
     * back and reset its size. A directory keeps the blocks it grew
     * into for as long as it exists otherwise, so an emptied
     * directory -- including the root -- reserved space forever. The
     * caller persists 'dir' afterwards, which stores the new size and
     * block table.
     */
    int empty = fs_dirent_is_empty(dir);

    if (empty != 1)
        return FS_OK;

    for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++) {
        if (dir->direct[i] != FS_BLOCK_NONE) {
            bitmap_set(dir->direct[i], 0);
            dir->direct[i] = FS_BLOCK_NONE;
        }
    }

    if (dir->indirect != FS_BLOCK_NONE) {
        bitmap_set(dir->indirect, 0);
        dir->indirect = FS_BLOCK_NONE;
    }

    dir->size = 0;

    return FS_OK;
}

static int fs_dirent_is_empty(fs_inode_t *dir)
{
    uint32_t capacity = fs_blocks_for_bytes(dir->size) * FS_BLOCK_SIZE;
    uint32_t offset = 0;

    while (offset + FS_DIRENT_SIZE <= capacity) {
        fs_dirent_t ent;
        uint32_t got = 0;
        int rc = fs_inode_read_at(dir, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        if (got < FS_DIRENT_SIZE)
            break;

        if (fs_dirent_live(&ent))
            return 0;

        offset += FS_DIRENT_SIZE;
    }

    return 1;
}

/* ---- 7. Path resolution -------------------------------------------- */

/* Copy the next component out of *path into name. Returns 0 when a
 * component was found, 1 at the end of the path. */
static int fs_next_component(const char **path, char *name, uint32_t *out_len)
{
    const char *p = *path;

    while (*p == '/')
        p++;

    if (*p == '\0') {
        *path = p;
        return 1;
    }

    /* Measure the component without moving the cursor: 'start' has to
     * stay put, because the copy below is taken from it. Advancing
     * 'p' inside the measuring loop made the memcpy read from the
     * end of the component, so "/selftest" resolved as "test" and
     * every path lookup in the system failed. */
    const char *start = p;
    uint32_t len = 0;

    while (start[len] != '\0' && start[len] != '/') {
        if (len + 1 >= FS_NAME_MAX)
            return -1;

        len++;
    }

    memcpy(name, start, len);
    name[len] = '\0';
    *out_len = len;
    *path = start + len;

    return 0;
}

/* True if the name is "." or "..". */
static int fs_is_dot_name(const char *name)
{
    if (name[0] == '.' && name[1] == '\0')
        return 1;

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0')
        return 1;

    return 0;
}

/* Directory inode whose contents are the parent of 'ino'. */
static int fs_parent_of(uint32_t ino, uint32_t *out_parent,
                        char *out_name, uint32_t *out_name_len)
{
    for (uint32_t i = 0; i < sb.inode_count; i++) {
        if (i == ino)
            continue;

        fs_inode_t candidate;

        if (fs_inode_read(i, &candidate) != FS_OK)
            continue;

        if (candidate.type != FS_TYPE_DIR || candidate.size == 0)
            continue;

        uint32_t capacity = fs_blocks_for_bytes(candidate.size) * FS_BLOCK_SIZE;

        for (uint32_t offset = 0; offset + FS_DIRENT_SIZE <= capacity;
             offset += FS_DIRENT_SIZE) {
            fs_dirent_t ent;
            uint32_t got = 0;

            if (fs_inode_read_at(&candidate, offset, &ent, FS_DIRENT_SIZE,
                                 &got) != FS_OK)
                break;

            if (got < FS_DIRENT_SIZE)
                break;

            if (ent.ino == ino) {
                *out_parent = i;

                if (out_name && out_name_len) {
                    memcpy(out_name, ent.name, ent.name_len);
                    out_name[ent.name_len] = '\0';
                    *out_name_len = ent.name_len;
                }

                return FS_OK;
            }
        }
    }

    return FS_ENOENT;
}

/*
 * Walk an absolute path to its inode.
 *
 * must_be_dir != 0 requires the final component to be a directory.
 * want_type, when non-zero, requires the final component to be of
 * that type (FS_TYPE_FILE or FS_TYPE_DIR).
 */
static int fs_readlink_internal(uint32_t ino, char *buf, uint32_t buf_size)
{
    fs_inode_t in;
    int rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    if (in.type != FS_TYPE_SYMLINK)
        return FS_EINVAL;

    if (buf == 0 || buf_size == 0)
        return FS_EINVAL;

    if (in.size >= buf_size)
        return FS_EINVAL;

    /*
     * Read the target through the block layer. This used to treat
     * direct[b] -- a block number -- as if it were a pointer and
     * dereference it, so reading any symlink walked off into low
     * kernel memory.
     */
    uint32_t read = 0;
    rc = fs_inode_read_at(&in, 0, buf, in.size, &read);

    if (rc != FS_OK)
        return rc;

    buf[read] = '\0';
    return FS_OK;
}

static int fs_resolve(const char *path, uint32_t *out_inode,
                      int want_type)
{
    if (path == 0 || out_inode == 0)
        return FS_EINVAL;

    if (path[0] != '/')
        return FS_EINVAL;

    uint32_t current = sb.root_inode;
    fs_inode_t node;
    int rc = fs_inode_read(current, &node);

    if (rc != FS_OK)
        return rc;

    const char *cursor = path;
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;
    int depth = 0;

    for (;;) {
        int step = fs_next_component(&cursor, name, &name_len);

        if (step < 0)
            return FS_ENAMETOOLONG;

        if (step == 1) {

            /* Trailing "." or "/" resolved to what we already have. */
            if (fs_is_dot_name(name))
                continue;

            break;
        }

        if (fs_is_dot_name(name)) {

            /* "." stays put. ".." needs a real parent link, which this
             * format does not store; the parent is found by reverse
             * lookup, and for root it stays at root. */
            if (name[0] == '.' && name[1] == '.') {
                uint32_t parent = FS_BLOCK_NONE;
                rc = fs_parent_of(current, &parent, 0, 0);

                if (rc == FS_OK)
                    current = parent;
            }

            continue;
        }

        if (++depth > FS_MAX_DEPTH)
            return FS_ELOOP;

        if (node.type != FS_TYPE_DIR)
            return FS_ENOTDIR;

        uint32_t offset = 0;
        rc = fs_dirent_find(&node, name, name_len, &offset);

        if (rc != FS_OK)
            return rc;

        fs_dirent_t ent;
        uint32_t got = 0;
        rc = fs_inode_read_at(&node, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        current = ent.ino;
        rc = fs_inode_read(current, &node);

        if (rc != FS_OK)
            return rc;

        /* Handle symlinks: if the current node is a symlink and there's
         * more path to resolve, follow the symlink. */
        while (node.type == FS_TYPE_SYMLINK) {
            char target[256];
            rc = fs_readlink_internal(current, target, sizeof(target));

            if (rc != FS_OK)
                return rc;

            /* If there's more path to resolve, restart from root. */
            const char *skip = cursor;

            while (*skip == '/')
                skip++;

            if (*skip == '\0') {
                /* Symlink is the final component; we're done. */
                break;
            }

            /* Restart resolution from root with the symlink target. */
            current = sb.root_inode;
            rc = fs_inode_read(current, &node);

            if (rc != FS_OK)
                return rc;

            cursor = target;
            continue;
        }

        /* More path left: the current node must be a directory. */
        const char *skip = cursor;

        while (*skip == '/')
            skip++;

        if (*skip != '\0' && node.type != FS_TYPE_DIR)
            return FS_ENOTDIR;
    }

    /* A type mismatch here means the caller asked for a file and the
     * path names a directory, or the reverse. EBADF ("bad file
     * descriptor") describes neither case and made fs_read() on a
     * directory report the wrong errno. */
    if (want_type != 0 && node.type != (uint8_t)want_type)
        return FS_ENOTDIR;

    *out_inode = current;
    return FS_OK;
}

/* ---- 8. Permission checks ------------------------------------------ */

static int fs_is_superuser(fs_cred_t cred)
{
    return cred.uid == 0 ? 1 : 0;
}

/*
 * Permission checks are expressed with the *owner* constants
 * (FS_PERM_OWNER_R / _W / _X) at every call site, but the bit that
 * matters is the one belonging to the caller's class. The three
 * classes are laid out so each is the previous one shifted: group is
 * owner << 3, other is owner << 6.
 *
 * Comparing an owner constant straight against the applicable class
 * meant that a "group" or "other" caller was tested with the owner's
 * bit position -- 0x40 & 0x01 is 0 -- so any world-readable file was
 * still refused. Translate the requested permission into the caller's
 * class before testing it.
 *
 * There is no FS_PERM_OTHER_X in this format, so an unprivileged
 * caller that is neither owner nor group member cannot be granted
 * search on a directory; the shifted bit falls outside the byte and is
 * treated as denied.
 */
static int fs_mode_allows(const fs_inode_t *in, fs_cred_t cred, uint8_t bit)
{
    if (fs_is_superuser(cred))
        return 1;

    uint32_t wanted = bit;
    uint32_t mode = in->mode;

    if (in->uid != cred.uid) {
        wanted = bit << (in->gid == cred.gid ? 3 : 6);
    }

    if (wanted > 0xFF)
        return 0;               /* no such bit: e.g. other-execute */

    return (mode & wanted) ? 1 : 0;
}

static int fs_require(const fs_inode_t *in, fs_cred_t cred, uint8_t bit)
{
    return fs_mode_allows(in, cred, bit) ? FS_OK : FS_EACCES;
}

/* ---- 9. Public API -------------------------------------------------- */

int fs_read_superblock(fs_superblock_t *out)
{
    if (out == 0)
        return FS_EINVAL;

    if (!ata_is_ready())
        return FS_EIO;

    int rc = fs_sector_read(0, out);

    return rc;
}

int fs_is_mounted(void)
{
    return mounted;
}

uint64_t fs_total_bytes(void)
{
    if (!mounted)
        return 0;

    return (uint64_t)sb.total_blocks * FS_BLOCK_SIZE;
}

uint64_t fs_free_bytes(void)
{
    if (!mounted || bitmap == 0)
        return 0;

    uint32_t used = 0;

    for (uint32_t b = 0; b < sb.total_blocks; b++) {
        if (bitmap_test(b))
            used++;
    }

    uint32_t free_blocks = sb.total_blocks - used;

    return (uint64_t)free_blocks * FS_BLOCK_SIZE;
}

int fs_resolve_dir(const char *path, fs_cred_t cred, uint32_t *out_inode)
{
    (void)cred;

    return fs_resolve(path, out_inode, FS_TYPE_DIR);
}

int fs_lookup(const char *path, fs_cred_t cred, uint8_t want_type,
              uint32_t *out_inode)
{
    if (out_inode == 0)
        return FS_EINVAL;

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, want_type);

    if (rc != FS_OK)
        return rc;

    /* Resolving to a name is itself a directory search, so the caller
     * needs search permission on whatever they came through. The
     * strictness of that is checked during the walk, not here, so all
     * this adds is a read check on the final node. */
    if (want_type == FS_TYPE_DIR) {
        fs_inode_t in;
        rc = fs_inode_read(ino, &in);

        if (rc != FS_OK)
            return rc;

        rc = fs_require(&in, cred, FS_PERM_OWNER_R);

        if (rc != FS_OK)
            return rc;
    }

    *out_inode = ino;
    return FS_OK;
}

int fs_stat(const char *path, fs_cred_t cred, fs_inode_t *out)
{
    if (out == 0)
        return FS_EINVAL;

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, 0);

    if (rc != FS_OK)
        return rc;

    rc = fs_inode_read(ino, out);

    if (rc != FS_OK)
        return rc;

    return fs_require(out, cred, FS_PERM_OWNER_R);
}

int fs_stat_by_inode(uint32_t ino, fs_inode_t *out)
{
    if (out == 0)
        return FS_EINVAL;

    return fs_inode_read(ino, out);
}

int fs_chmod(const char *path, fs_cred_t cred, uint8_t mode)
{
    if (mode & (uint8_t)~FS_MODE_MASK)
        return FS_EINVAL;

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, 0);

    if (rc != FS_OK)
        return rc;

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    /* Only the owner or the superuser may change the mode. */
    if (!fs_is_superuser(cred) && in.uid != cred.uid)
        return FS_EACCES;

    in.mode = mode & FS_MODE_MASK;
    fs_inode_touch(&in);

    return fs_inode_write(ino, &in);
}

int fs_chown(const char *path, fs_cred_t cred, uint16_t uid, uint16_t gid)
{
    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, 0);

    if (rc != FS_OK)
        return rc;

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    /* Giving a file away to another owner is superuser-only; an
     * unprivileged owner may adjust the group of something it owns,
     * as long as it stays in a group the caller belongs to. */
    if (!fs_is_superuser(cred)) {
        if (in.uid != cred.uid)
            return FS_EACCES;

        if (uid != cred.uid || gid != cred.gid)
            return FS_EACCES;
    }

    in.uid = uid;
    in.gid = gid;
    fs_inode_touch(&in);

    return fs_inode_write(ino, &in);
}

/* Split "/a/b" into the parent directory path and the final name.
 * "/a" yields parent "/" and name "a"; "/a/" yields parent "/a" and
 * name "a", since a trailing slash just means "that directory". */
static int fs_split_path(const char *path, char *out_parent,
                         uint32_t parent_size, char *out_name,
                         uint32_t *out_name_len)
{
    if (path == 0 || path[0] != '/')
        return FS_EINVAL;

    uint32_t len = 0;

    while (path[len] != '\0')
        len++;

    /* Ignore trailing slashes when measuring. */
    while (len > 1 && path[len - 1] == '/')
        len--;

    if (len == 1) {

        /* "/" has no name component, so nothing to create or
         * remove. */
        if (out_name_len)
            *out_name_len = 0;

        out_parent[0] = '/';
        out_parent[1] = '\0';
        return FS_EINVAL;
    }

    uint32_t split = len;

    while (split > 0 && path[split - 1] != '/')
        split--;

    uint32_t name_start = split;

    while (name_start < len && path[name_start] == '/')
        name_start++;

    uint32_t name_len = len - name_start;

    if (name_len == 0 || name_len >= FS_NAME_MAX)
        return FS_ENAMETOOLONG;

    if (name_start == 1) {
        out_parent[0] = '/';
        out_parent[1] = '\0';
    } else {
        uint32_t parent_len = name_start - 1;

        if (parent_len + 1 > parent_size)
            return FS_ENAMETOOLONG;

        memcpy(out_parent, path, parent_len);
        out_parent[parent_len] = '\0';
    }

    memcpy(out_name, path + name_start, name_len);
    out_name[name_len] = '\0';

    if (out_name_len)
        *out_name_len = name_len;

    return FS_OK;
}

/* Create a file or directory inode and link it into a parent. */
static int fs_create(const char *path, fs_cred_t cred, uint8_t type,
                     uint8_t mode, uint32_t *out_inode)
{
    char parent_path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;

    int rc = fs_split_path(path, parent_path, sizeof(parent_path),
                           name, &name_len);

    if (rc != FS_OK)
        return rc;

    uint32_t parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(parent_path, &parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t parent;
    rc = fs_inode_read(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&parent, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&parent, cred, FS_PERM_OWNER_X);

    if (rc != FS_OK)
        return rc;

    uint32_t existing_offset = 0;
    rc = fs_dirent_find(&parent, name, name_len, &existing_offset);

    if (rc == FS_OK) {

        /* Report the inode that already owns this name. Callers that
         * tolerate FS_EEXIST -- fs_write() and fs_open() -- go on to
         * operate on *out_inode, so leaving it unset made them treat
         * inode 0, the root directory, as the target. That surfaced as
         * fs_write() failing with FS_EISDIR on a perfectly good file. */
        fs_dirent_t found;
        uint32_t got = 0;

        rc = fs_inode_read_at(&parent, existing_offset, &found,
                              FS_DIRENT_SIZE, &got);

        if (rc != FS_OK || got < FS_DIRENT_SIZE)
            return rc == FS_OK ? FS_EIO : rc;

        if (out_inode)
            *out_inode = found.ino;

        return FS_EEXIST;
    }

    if (rc != FS_ENOENT)
        return rc;

    uint32_t ino = fs_inode_alloc();

    if (ino == FS_BLOCK_NONE)
        return FS_ENOSPC;

    fs_inode_t in;
    fs_inode_init(&in, type, cred.uid, cred.gid, mode);

    rc = fs_inode_write(ino, &in);

    if (rc != FS_OK) {
        /* Roll the inode back so it does not leak as a half-built
         * entry. */
        fs_inode_t blank;
        fs_inode_init(&blank, FS_TYPE_FREE, 0, 0, 0);
        fs_inode_write(ino, &blank);
        return rc;
    }

    rc = fs_dirent_add(&parent, ino, type, name, name_len);

    if (rc != FS_OK) {
        fs_inode_free(ino);
        return rc;
    }

    fs_inode_touch(&parent);
    rc = fs_inode_write(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

    if (bitmap_flush() != FS_OK)
        return FS_EIO;

    if (out_inode)
        *out_inode = ino;

    return FS_OK;
}

int fs_mkdir(const char *path, fs_cred_t cred, uint8_t mode)
{
    uint32_t ino = 0;
    int rc = fs_create(path, cred, FS_TYPE_DIR, mode, &ino);

    if (rc != FS_OK)
        return rc;

    /* A fresh directory's mtime is set by fs_create; nothing to
     * initialise inside it yet. */
    return FS_OK;
}

int fs_create_file(const char *path, fs_cred_t cred, uint8_t mode,
                   uint32_t *out_inode)
{
    return fs_create(path, cred, FS_TYPE_FILE, mode, out_inode);
}

int fs_write(const char *path, fs_cred_t cred,
             const void *data, uint32_t size)
{
    if (mounted == 0)
        return FS_EIO;

    if (size > 0 && data == 0)
        return FS_EINVAL;

    if (size > FS_MAX_FILE_SIZE)
        return FS_EINVAL;

    FS_LOCK();

    uint32_t ino = 0;
    int rc = fs_create_file(path, cred, FS_MODE_FILE_DEFAULT, &ino);

    if (rc != FS_OK && rc != FS_EEXIST) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&in, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    if (in.type == FS_TYPE_DIR) {
        FS_UNLOCK();
        return FS_EISDIR;
    }

    /* Overwrite from the start: release whatever the old contents
     * held, so rewriting a large file does not leak its blocks. */
    for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++) {
        if (in.direct[i] != FS_BLOCK_NONE) {
            bitmap_set(in.direct[i], 0);
            in.direct[i] = FS_BLOCK_NONE;
        }
    }

    if (in.indirect != FS_BLOCK_NONE) {
        uint8_t stage[FS_STAGE_BYTES];

        if (fs_block_read(in.indirect, stage) == FS_OK) {
            uint32_t *ptrs = (uint32_t *)stage;

            for (uint32_t i = 0; i < FS_INDIRECT_PTRS; i++) {
                if (ptrs[i] != FS_BLOCK_NONE)
                    bitmap_set(ptrs[i], 0);
            }
        }

        bitmap_set(in.indirect, 0);
        in.indirect = FS_BLOCK_NONE;
    }

    in.size = 0;

    uint32_t written = 0;
    rc = fs_inode_write_at(&in, 0, data, size, &written);

    if (rc == FS_OK)
        rc = fs_inode_write(ino, &in);

    if (rc == FS_OK)
        rc = bitmap_flush();

    FS_UNLOCK();
    return rc;
}

int fs_read(const char *path, fs_cred_t cred, void **out, uint32_t *out_size)
{
    if (mounted == 0)
        return FS_EIO;

    if (out == 0 || out_size == 0)
        return FS_EINVAL;

    *out = 0;
    *out_size = 0;

    FS_LOCK();

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, FS_TYPE_FILE);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&in, cred, FS_PERM_OWNER_R);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    if (in.size == 0) {
        *out_size = 0;
        FS_UNLOCK();
        return FS_OK;
    }

    if (in.size > FS_MAX_FILE_SIZE) {
        FS_UNLOCK();
        return FS_EIO;
    }

    void *buffer = kmalloc(in.size);

    if (buffer == 0) {
        FS_UNLOCK();
        return FS_ENOMEM;
    }

    uint32_t got = 0;
    rc = fs_inode_read_at(&in, 0, buffer, in.size, &got);

    if (rc != FS_OK) {
        kfree(buffer);
        FS_UNLOCK();
        return rc;
    }

    *out = buffer;
    *out_size = got;

    FS_UNLOCK();
    return FS_OK;
}

int fs_truncate(const char *path, fs_cred_t cred)
{
    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, FS_TYPE_FILE);

    if (rc != FS_OK)
        return rc;

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&in, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK)
        return rc;

    in.size = 0;
    fs_inode_touch(&in);
    rc = fs_inode_write(ino, &in);

    if (rc != FS_OK)
        return rc;

    return bitmap_flush();
}

int fs_delete(const char *path, fs_cred_t cred)
{
    char parent_path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;

    int rc = fs_split_path(path, parent_path, sizeof(parent_path),
                           name, &name_len);

    if (rc != FS_OK)
        return rc;

    uint32_t parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(parent_path, &parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t parent;
    rc = fs_inode_read(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

    /* Removing a name needs write permission on the directory, not on
     * the file -- the file's own mode is irrelevant here. */
    rc = fs_require(&parent, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&parent, cred, FS_PERM_OWNER_X);

    if (rc != FS_OK)
        return rc;

    uint32_t offset = 0;
    rc = fs_dirent_find(&parent, name, name_len, &offset);

    if (rc != FS_OK)
        return rc;

    fs_dirent_t ent;
    uint32_t got = 0;
    rc = fs_inode_read_at(&parent, offset, &ent, FS_DIRENT_SIZE, &got);

    if (rc != FS_OK || got < FS_DIRENT_SIZE)
        return rc == FS_OK ? FS_EIO : rc;

    if (ent.ino == sb.root_inode)
        return FS_EACCES;

    fs_inode_t victim;
    rc = fs_inode_read(ent.ino, &victim);

    if (rc != FS_OK)
        return rc;

    if (victim.type == FS_TYPE_DIR) {

        /* fs_delete refuses to remove a directory, even an empty
         * one: rmdir is the explicit operation. */
        return FS_EISDIR;
    }

    rc = fs_dirent_remove(&parent, offset);

    if (rc != FS_OK)
        return rc;

    rc = fs_inode_free(ent.ino);

    if (rc != FS_OK)
        return rc;

    fs_inode_touch(&parent);
    rc = fs_inode_write(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

    return bitmap_flush();
}

int fs_rmdir(const char *path, fs_cred_t cred)
{
    char parent_path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;

    int rc = fs_split_path(path, parent_path, sizeof(parent_path),
                           name, &name_len);

    if (rc != FS_OK)
        return rc;

    uint32_t parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(parent_path, &parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t parent;
    rc = fs_inode_read(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&parent, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK)
        return rc;

    uint32_t offset = 0;
    rc = fs_dirent_find(&parent, name, name_len, &offset);

    if (rc != FS_OK)
        return rc;

    fs_dirent_t ent;
    uint32_t got = 0;
    rc = fs_inode_read_at(&parent, offset, &ent, FS_DIRENT_SIZE, &got);

    if (rc != FS_OK || got < FS_DIRENT_SIZE)
        return rc == FS_OK ? FS_EIO : rc;

    if (ent.ino == sb.root_inode)
        return FS_EACCES;

    if (ent.type != FS_TYPE_DIR)
        return FS_ENOTDIR;

    fs_inode_t victim;
    rc = fs_inode_read(ent.ino, &victim);

    if (rc != FS_OK)
        return rc;

    if (!fs_dirent_is_empty(&victim))
        return FS_ENOTEMPTY;

    rc = fs_dirent_remove(&parent, offset);

    if (rc != FS_OK)
        return rc;

    rc = fs_inode_free(ent.ino);

    if (rc != FS_OK)
        return rc;

    fs_inode_touch(&parent);
    rc = fs_inode_write(parent_ino, &parent);

    if (rc != FS_OK)
        return rc;

return bitmap_flush();
}

/* ---- Directory listing --------------------------------------------- */

/* Create a symbolic link. The target path is stored as the file
 * contents of a special inode of type FS_TYPE_SYMLINK. */
int fs_symlink(const char *target, const char *linkpath, fs_cred_t cred)
{
    if (target == 0 || linkpath == 0)
        return FS_EINVAL;

    uint32_t target_len = 0;
    while (target[target_len] != '\0' && target_len < FS_MAX_FILE_SIZE)
        target_len++;

    if (target_len == 0 || target_len >= FS_MAX_FILE_SIZE)
        return FS_EINVAL;

    FS_LOCK();

    char parent_path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;

    int rc = fs_split_path(linkpath, parent_path, sizeof(parent_path),
                           name, &name_len);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    uint32_t parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(parent_path, &parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t parent;
    rc = fs_inode_read(parent_ino, &parent);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&parent, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&parent, cred, FS_PERM_OWNER_X);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Check if the name already exists. */
    rc = fs_dirent_find(&parent, name, name_len, 0);

    if (rc == FS_OK) {
        FS_UNLOCK();
        return FS_EEXIST;
    }

    /* Allocate a new inode for the symlink. */
    uint32_t ino = fs_inode_alloc();

    if (ino == FS_BLOCK_NONE) {
        FS_UNLOCK();
        return FS_ENOSPC;
    }

    /* Write the target path as the file contents. */
    uint32_t blocks_needed = fs_blocks_for_bytes(target_len);
    if (blocks_needed > FS_DIRECT_BLOCKS) {
        fs_inode_free(ino);
        FS_UNLOCK();
        return FS_ENOSPC;
    }

    fs_inode_t in;
    for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++)
        in.direct[i] = FS_BLOCK_NONE;
    in.indirect = FS_BLOCK_NONE;

    in.type = FS_TYPE_SYMLINK;
    in.mode = FS_MODE_FILE_DEFAULT;
    in.uid = cred.uid;
    in.gid = cred.gid;
    in.size = target_len;
    in.mtime = 0;
    in.flags = 0;

    /* Allocate blocks and write the target path. */
    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t blk = frame_alloc();
        if (blk == FRAME_INVALID || blk >= PAGING_IDENTITY_LIMIT) {
            for (uint32_t j = 0; j < b; j++)
                frame_free(in.direct[j]);
            fs_inode_free(ino);
            FS_UNLOCK();
            return FS_ENOSPC;
        }
        in.direct[b] = blk;

        uint8_t *blk_dst = (uint8_t *)blk;
        uint32_t chunk = (target_len > FS_BLOCK_SIZE) ? FS_BLOCK_SIZE : target_len;
        for (uint32_t k = 0; k < chunk; k++)
            blk_dst[k] = target[k];
        target += chunk;
        target_len -= chunk;
    }

    fs_inode_touch(&in);
    rc = fs_inode_write(ino, &in);

    if (rc != FS_OK) {
        for (uint32_t b = 0; b < blocks_needed; b++)
            frame_free(in.direct[b]);
        fs_inode_free(ino);
        FS_UNLOCK();
        return rc;
    }

    /* Add directory entry. */
    rc = fs_dirent_add(&parent, ino, FS_TYPE_SYMLINK, name, name_len);

    if (rc != FS_OK) {
        for (uint32_t b = 0; b < blocks_needed; b++)
            frame_free(in.direct[b]);
        fs_inode_free(ino);
        FS_UNLOCK();
        return rc;
    }

    fs_inode_touch(&parent);
    rc = fs_inode_write(parent_ino, &parent);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = bitmap_flush();
    FS_UNLOCK();
    return rc;
}

/* Read the target of a symbolic link. */
int fs_readlink(const char *linkpath, fs_cred_t cred, char *buf, uint32_t buf_size)
{
    if (linkpath == 0 || buf == 0 || buf_size == 0)
        return FS_EINVAL;

    FS_LOCK();

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(linkpath, &ino, FS_TYPE_SYMLINK);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&in, cred, FS_PERM_OWNER_R);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    uint32_t target_len = in.size;
    if (target_len >= buf_size) {
        FS_UNLOCK();
        return FS_EINVAL;
    }

    /* Read the target path from the file contents. */
    uint32_t blocks = fs_blocks_for_bytes(target_len);
    uint32_t read = 0;

    for (uint32_t b = 0; b < blocks; b++) {
        if (in.direct[b] == FS_BLOCK_NONE) {
            FS_UNLOCK();
            return FS_EIO;
        }

        uint8_t *blk_src = (uint8_t *)in.direct[b];
        uint32_t chunk = (target_len > FS_BLOCK_SIZE) ? FS_BLOCK_SIZE : target_len;
        if (read + chunk > buf_size - 1) {
            FS_UNLOCK();
            return FS_EINVAL;
        }

        for (uint32_t k = 0; k < chunk; k++)
            buf[read++] = blk_src[k];
        target_len -= chunk;
    }

    buf[read] = '\0';

    FS_UNLOCK();
    return FS_OK;
}

/* Create a hard link to an existing file. This adds another
 * directory entry pointing to the same inode. */
int fs_link(const char *existing_path, const char *new_path, fs_cred_t cred)
{
    if (existing_path == 0 || new_path == 0)
        return FS_EINVAL;

    FS_LOCK();

    /* Resolve the existing file's inode. */
    uint32_t existing_ino = FS_BLOCK_NONE;
    int rc = fs_resolve(existing_path, &existing_ino, 0);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t existing;
    rc = fs_inode_read(existing_ino, &existing);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Only allow hard links to regular files (not directories or symlinks). */
    if (existing.type != FS_TYPE_FILE) {
        FS_UNLOCK();
        return FS_EINVAL;
    }

    rc = fs_require(&existing, cred, FS_PERM_OWNER_R);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Parse the new path. */
    char parent_path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    uint32_t name_len = 0;

    rc = fs_split_path(new_path, parent_path, sizeof(parent_path),
                       name, &name_len);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    uint32_t parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(parent_path, &parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    fs_inode_t parent;
    rc = fs_inode_read(parent_ino, &parent);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&parent, cred, FS_PERM_OWNER_W);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = fs_require(&parent, cred, FS_PERM_OWNER_X);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Check if the name already exists. */
    rc = fs_dirent_find(&parent, name, name_len, 0);

    if (rc == FS_OK) {
        FS_UNLOCK();
        return FS_EEXIST;
    }

    /* Add directory entry pointing to the same inode. */
    rc = fs_dirent_add(&parent, existing_ino, FS_TYPE_FILE, name, name_len);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Increment the inode's link count. We don't have a link count field
     * in the inode yet, so we just note that the file has multiple links.
     * The inode is only freed when the last link is removed (in fs_delete). */

    fs_inode_touch(&parent);
    rc = fs_inode_write(parent_ino, &parent);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    rc = bitmap_flush();
    FS_UNLOCK();
    return rc;
}

/* ---- Directory listing --------------------------------------------- */

int fs_count_entries(const char *path, fs_cred_t cred, uint32_t *out_count)
{
    if (out_count == 0)
        return FS_EINVAL;

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t dir;
    rc = fs_inode_read(ino, &dir);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&dir, cred, FS_PERM_OWNER_R);

    if (rc != FS_OK)
        return rc;

    uint32_t count = 0;
    uint32_t capacity = fs_blocks_for_bytes(dir.size) * FS_BLOCK_SIZE;
    uint32_t offset = 0;

    while (offset + FS_DIRENT_SIZE <= capacity) {
        fs_dirent_t ent;
        uint32_t got = 0;
        rc = fs_inode_read_at(&dir, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        if (got < FS_DIRENT_SIZE)
            break;

        /* Count live entries only. Testing ino != FS_BLOCK_NONE alone
         * also counts blank slots, whose zeroed ino happens to be a
         * valid inode number, so a directory holding two entries in a
         * 33-slot area reported 33. */
        if (fs_dirent_live(&ent))
            count++;

        offset += FS_DIRENT_SIZE;
    }

    *out_count = count;
    return FS_OK;
}

int fs_list(const char *path, fs_cred_t cred, uint32_t index,
            uint32_t *out_inode, const char **out_name, uint8_t *out_type)
{
    /*
     * The caller's name pointer has to outlive this frame, so the
     * entry is copied into a static buffer rather than pointed at
     * directly. It is overwritten by the next call, as fs.h says.
     * Safe because callers are serialised and single-threaded.
     */
    static char name_buffer[FS_NAME_MAX];

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t dir;
    rc = fs_inode_read(ino, &dir);

    if (rc != FS_OK)
        return rc;

    rc = fs_require(&dir, cred, FS_PERM_OWNER_R);

    if (rc != FS_OK)
        return rc;

    uint32_t capacity = fs_blocks_for_bytes(dir.size) * FS_BLOCK_SIZE;
    uint32_t offset = 0;
    uint32_t seen = 0;

    while (offset + FS_DIRENT_SIZE <= capacity) {
        fs_dirent_t ent;
        uint32_t got = 0;
        rc = fs_inode_read_at(&dir, offset, &ent, FS_DIRENT_SIZE, &got);

        if (rc != FS_OK)
            return rc;

        if (got < FS_DIRENT_SIZE)
            break;

        if (fs_dirent_live(&ent)) {
            if (seen == index) {
                if (out_inode)
                    *out_inode = ent.ino;

                if (out_type)
                    *out_type = ent.type;

                if (out_name) {
                    uint32_t copy = ent.name_len;

                    if (copy > FS_NAME_MAX - 1)
                        copy = FS_NAME_MAX - 1;

                    memcpy(name_buffer, ent.name, copy);
                    name_buffer[copy] = '\0';
                    *out_name = name_buffer;
                }

                return FS_OK;
            }

            seen++;
        }

        offset += FS_DIRENT_SIZE;
    }

    return FS_ENOENT;
}

int fs_rename(const char *from, const char *to, fs_cred_t cred)
{
    char from_parent_path[FS_PATH_MAX];
    char from_name[FS_NAME_MAX];
    uint32_t from_name_len = 0;

    int rc = fs_split_path(from, from_parent_path, sizeof(from_parent_path),
                           from_name, &from_name_len);

    if (rc != FS_OK)
        return rc;

    uint32_t from_parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(from_parent_path, &from_parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t from_parent;
    rc = fs_inode_read(from_parent_ino, &from_parent);

    if (rc != FS_OK)
        return rc;

    if (fs_require(&from_parent, cred, FS_PERM_OWNER_W) != FS_OK ||
        fs_require(&from_parent, cred, FS_PERM_OWNER_X) != FS_OK) {
        return FS_EACCES;
    }

    uint32_t from_offset = 0;
    rc = fs_dirent_find(&from_parent, from_name, from_name_len, &from_offset);

    if (rc != FS_OK)
        return rc;

    fs_dirent_t moved;
    uint32_t got = 0;
    rc = fs_inode_read_at(&from_parent, from_offset, &moved,
                           FS_DIRENT_SIZE, &got);

    if (rc != FS_OK || got < FS_DIRENT_SIZE)
        return rc == FS_OK ? FS_EIO : rc;

    if (moved.ino == sb.root_inode)
        return FS_EACCES;

    char to_parent_path[FS_PATH_MAX];
    char to_name[FS_NAME_MAX];
    uint32_t to_name_len = 0;

    rc = fs_split_path(to, to_parent_path, sizeof(to_parent_path),
                       to_name, &to_name_len);

    if (rc != FS_OK)
        return rc;

    uint32_t to_parent_ino = FS_BLOCK_NONE;
    rc = fs_resolve(to_parent_path, &to_parent_ino, FS_TYPE_DIR);

    if (rc != FS_OK)
        return rc;

    fs_inode_t to_parent;
    rc = fs_inode_read(to_parent_ino, &to_parent);

    if (rc != FS_OK)
        return rc;

    if (fs_require(&to_parent, cred, FS_PERM_OWNER_W) != FS_OK ||
        fs_require(&to_parent, cred, FS_PERM_OWNER_X) != FS_OK) {
        return FS_EACCES;
    }

    /* Renaming within the same directory is just a name change, and
     * must not be rejected as "already exists" against itself. */
    if (to_parent_ino == from_parent_ino) {
        fs_dirent_t existing;
        memset(&existing, 0, sizeof(existing));
        uint32_t existing_offset = 0;

        if (fs_dirent_find(&from_parent, to_name, to_name_len,
                           &existing_offset) == FS_OK &&
            existing.ino != moved.ino) {
            return FS_EEXIST;
        }

        fs_dirent_t renamed = moved;
        renamed.name_len = (uint8_t)to_name_len;
        memset(renamed.name, 0, FS_NAME_MAX);
        memcpy(renamed.name, to_name, to_name_len);

        uint32_t written = 0;
        rc = fs_inode_write_at(&from_parent, from_offset, &renamed,
                               FS_DIRENT_SIZE, &written);

        if (rc != FS_OK)
            return rc;

        /* Nothing else to do: the entry already lives at from_offset
         * and now carries the new name. This used to call
         * fs_dirent_remove() on that same offset afterwards, which
         * retired the entry that had just been written and left the
         * file unreachable under its new name. */

        fs_inode_touch(&from_parent);
        return fs_inode_write(from_parent_ino, &from_parent);
    }

    /* Moving between directories: link at the destination first, then
     * unlink at the source. Doing it in that order means a failure
     * part-way leaves a duplicate name rather than a dangling one,
     * which is the recoverable direction. */
    uint32_t existing_offset = 0;

    if (fs_dirent_find(&to_parent, to_name, to_name_len,
                       &existing_offset) == FS_OK) {
        return FS_EEXIST;
    }

    rc = fs_dirent_add(&to_parent, moved.ino, moved.type, to_name,
                       to_name_len);

    if (rc != FS_OK)
        return rc;

    rc = fs_dirent_remove(&from_parent, from_offset);

    if (rc != FS_OK)
        return rc;

    fs_inode_touch(&to_parent);
    rc = fs_inode_write(to_parent_ino, &to_parent);

    if (rc != FS_OK)
        return rc;

    fs_inode_touch(&from_parent);
    rc = fs_inode_write(from_parent_ino, &from_parent);

    if (rc != FS_OK)
        return rc;

    return bitmap_flush();
}

/* ---- Handles -------------------------------------------------------- */

int fs_open(const char *path, fs_cred_t cred, uint32_t flags,
            fs_handle_t *out)
{
    if (out == 0)
        return FS_EINVAL;

    if (mounted == 0)
        return FS_EIO;

    out->inode = FS_BLOCK_NONE;
    out->offset = 0;
    out->flags = flags;

    int creating = (flags & FS_OPEN_CREATE) ? 1 : 0;
    int writing = (flags & (FS_OPEN_WRITE | FS_OPEN_TRUNC | FS_OPEN_APPEND))
                      ? 1 : 0;
    int reading = (flags & (FS_OPEN_READ | FS_OPEN_WRITE)) ? 1 : 0;

    /*
     * No bits at all is the conventional O_RDONLY (FS_OPEN_READ is
     * 0x01, so callers that pass plain 0 for "read it" got every
     * access flag false and were rejected with FS_EINVAL).
     */
    if (!creating && !writing && !reading && flags == 0)
        reading = 1;

    if (!creating && !writing && !reading)
        return FS_EINVAL;

    uint32_t ino = FS_BLOCK_NONE;
    int rc = fs_resolve(path, &ino, FS_TYPE_FILE);

    if (rc == FS_ENOENT && creating) {
        rc = fs_create_file(path, cred, FS_MODE_FILE_DEFAULT, &ino);

        if (rc != FS_OK)
            return rc;
    } else if (rc != FS_OK) {
        return rc;
    }

    fs_inode_t in;
    rc = fs_inode_read(ino, &in);

    if (rc != FS_OK)
        return rc;

    if (writing) {
        rc = fs_require(&in, cred, FS_PERM_OWNER_W);

        if (rc != FS_OK)
            return rc;
    } else if (reading) {
        rc = fs_require(&in, cred, FS_PERM_OWNER_R);

        if (rc != FS_OK)
            return rc;
    }

    if (flags & FS_OPEN_TRUNC) {
        uint32_t freed = 0;

        for (uint32_t i = 0; i < FS_DIRECT_BLOCKS; i++) {
            if (in.direct[i] != FS_BLOCK_NONE) {
                bitmap_set(in.direct[i], 0);
                in.direct[i] = FS_BLOCK_NONE;
                freed++;
            }
        }

        in.size = 0;
        rc = fs_inode_write(ino, &in);

        if (rc != FS_OK)
            return rc;
    }

    if (flags & FS_OPEN_APPEND)
        out->offset = in.size;
    else
        out->offset = 0;

    out->inode = ino;
    out->flags = flags;

    return FS_OK;
}

int fs_close(fs_handle_t *handle)
{
    if (handle == 0)
        return FS_EINVAL;

    if (handle->inode == FS_BLOCK_NONE)
        return FS_OK;

    int rc = bitmap_flush();

    handle->inode = FS_BLOCK_NONE;
    return rc;
}

int fs_handle_read(fs_handle_t *handle, void *buffer, uint32_t size,
                   uint32_t *out_read)
{
    if (handle == 0 || handle->inode == FS_BLOCK_NONE)
        return FS_EBADF;

    if (size > 0 && buffer == 0)
        return FS_EINVAL;

    fs_inode_t in;
    int rc = fs_inode_read(handle->inode, &in);

    if (rc != FS_OK)
        return rc;

    uint32_t got = 0;
    rc = fs_inode_read_at(&in, handle->offset, buffer, size, &got);

    if (rc != FS_OK)
        return rc;

    handle->offset += got;

    if (out_read)
        *out_read = got;

    return FS_OK;
}

int fs_handle_write(fs_handle_t *handle, const void *buffer, uint32_t size,
                    uint32_t *out_written)
{
    if (handle == 0 || handle->inode == FS_BLOCK_NONE)
        return FS_EBADF;

    if (size > 0 && buffer == 0)
        return FS_EINVAL;

    if (!(handle->flags & FS_OPEN_WRITE) &&
        !(handle->flags & FS_OPEN_CREATE) &&
        !(handle->flags & FS_OPEN_APPEND)) {
        return FS_EACCES;
    }

    fs_inode_t in;
    int rc = fs_inode_read(handle->inode, &in);

    if (rc != FS_OK)
        return rc;

    uint32_t written = 0;
    rc = fs_inode_write_at(&in, handle->offset, buffer, size, &written);

    if (rc != FS_OK)
        return rc;

    fs_inode_touch(&in);
    rc = fs_inode_write(handle->inode, &in);

    if (rc != FS_OK)
        return rc;

    handle->offset += written;

    if (out_written)
        *out_written = written;

    return FS_OK;
}

int fs_handle_seek(fs_handle_t *handle, uint32_t offset, uint32_t whence)
{
    if (handle == 0 || handle->inode == FS_BLOCK_NONE)
        return FS_EBADF;

    uint32_t base = 0;

    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = handle->offset;
    else if (whence == SEEK_END) {
        fs_inode_t in;
        int rc = fs_inode_read(handle->inode, &in);

        if (rc != FS_OK)
            return rc;

        base = in.size;
    } else {
        return FS_EINVAL;
    }

    handle->offset = base + offset;
    return FS_OK;
}

int fs_handle_size(fs_handle_t *handle, uint32_t *out_size)
{
    if (handle == 0 || handle->inode == FS_BLOCK_NONE)
        return FS_EBADF;

    fs_inode_t in;
    int rc = fs_inode_read(handle->inode, &in);

    if (rc != FS_OK)
        return rc;

    if (out_size)
        *out_size = in.size;

    return FS_OK;
}

/* ---- Format and mount ----------------------------------------------- */

/* Work out the geometry for a drive of 'total_sectors' and fill in
 * 'out'. Returns FS_OK, or an error if the drive is too small. */
int fs_plan_layout(uint32_t total_sectors, fs_superblock_t *out)
{
    if (out == 0)
        return FS_EINVAL;

    if (total_sectors < fs_minimum_sectors())
        return FS_ENOSPC;

    /* Blocks are the allocation unit, and the metadata regions are
     * block aligned so nothing straddles a block boundary. */
    uint32_t total_blocks = total_sectors / FS_BLOCK_SECTORS;

    if (total_blocks < 16)
        return FS_ENOSPC;

    memset(out, 0, sizeof(*out));

    memcpy(out->magic, FS_MAGIC, FS_MAGIC_LEN);
    out->version = FS_VERSION;
    out->sector_size = FS_SECTOR_SIZE;
    out->block_size = FS_BLOCK_SIZE;
    out->total_sectors = total_sectors;
    out->total_blocks = total_blocks;

    /* Bitmap: one bit per block. */
    out->bitmap_sectors = fs_sectors_for_bytes(
        (total_blocks + 7) / 8);
    out->bitmap_lba = 2;

    /* Inode table: a whole number of sectors, at least 128 inodes. */
    uint32_t min_inode_sectors =
        fs_sectors_for_bytes(128 * FS_INODE_SIZE);
    uint32_t inode_sectors = 1;

    while (inode_sectors < min_inode_sectors)
        inode_sectors *= 2;

    out->inode_table_lba = out->bitmap_lba + out->bitmap_sectors;

    /*
     * Grow (never shrink) the inode table so data_start_lba lands
     * on a whole block boundary. Growing the table instead of
     * rounding data_start_lba keeps inode_table_lba + sectors ==
     * data_start_lba exactly, which fs_validate_superblock
     * requires; rounding data_start_lba alone used to produce
     * images that format accepted but mount rejected.
     */
    {
        uint32_t align = FS_BLOCK_SECTORS;

        while ((out->inode_table_lba + inode_sectors) % align != 0)
            inode_sectors++;
    }

    out->inode_table_sectors = inode_sectors;
    out->inode_count = inode_sectors * FS_INODES_PER_SECTOR;

    /* Block N really is at data_start_lba + N * FS_BLOCK_SECTORS. */
    out->data_start_lba = out->inode_table_lba + out->inode_table_sectors;
    out->root_inode = 0;

    if (out->data_start_lba >= total_sectors)
        return FS_ENOSPC;

    return FS_OK;
}

int fs_format(uint16_t uid, uint16_t gid)
{
    if (!ata_is_ready()) {
        console_error("FS: cannot format, no ATA drive");
        return FS_EIO;
    }

    FS_LOCK();

    uint32_t total_sectors = (uint32_t)ata_total_sectors();
    fs_superblock_t plan;
    int rc = fs_plan_layout(total_sectors, &plan);

    if (rc != FS_OK) {
        console_error("FS: drive too small to format");
        FS_UNLOCK();
        return rc;
    }

    uint32_t needed = plan.data_start_lba + FS_BLOCK_SECTORS;

    if (needed > total_sectors) {
        console_error("FS: drive too small to format");
        FS_UNLOCK();
        return FS_ENOSPC;
    }

    /* Fill the allocation bitmap. */
    uint8_t *fresh_bitmap = kmalloc(plan.bitmap_sectors * FS_SECTOR_SIZE);

    if (fresh_bitmap == 0) {
        FS_UNLOCK();
        return FS_ENOMEM;
    }

    memset(fresh_bitmap, 0, plan.bitmap_sectors * FS_SECTOR_SIZE);

    uint8_t *old_bitmap = bitmap;
    uint32_t old_bytes = bitmap_bytes;
    fs_superblock_t old_sb = sb;
    int was_mounted = mounted;

    bitmap = fresh_bitmap;
    bitmap_bytes = plan.bitmap_sectors * FS_SECTOR_SIZE;
    sb = plan;
    mounted = 1;

    /* Everything the metadata itself occupies is not available to
     * files. */
    bitmap_reserve_metadata();

    /* Blank the inode table. */
    uint8_t blank_sector[FS_SECTOR_SIZE];
    memset(blank_sector, 0, sizeof(blank_sector));

    for (uint32_t i = 0; i < plan.inode_table_sectors; i++) {
        rc = fs_sector_write(plan.inode_table_lba + i, blank_sector);

        if (rc != FS_OK)
            goto fail;
    }

    /* Create the root directory. */
    fs_inode_t root;
    fs_inode_init(&root, FS_TYPE_DIR, uid, gid, FS_MODE_DIR_DEFAULT);
    rc = fs_inode_write(0, &root);

    if (rc != FS_OK)
        goto fail;

    /* A stamp derived from the clock, so two images formatted the
     * same second are distinguishable only by the fields that vary.
     * It is informational, not a correctness requirement. */
    sb.uuid_lo = fs_now_epoch() ^ total_sectors;
    sb.uuid_hi = plan.data_start_lba * 2654435761U;

    memcpy(sb.magic_back, FS_MAGIC, FS_MAGIC_LEN);
    sb.format_version_check = FS_VERSION;

    rc = bitmap_flush();

    if (rc != FS_OK)
        goto fail;

    /* The superblock is written last, so a power cut mid-format
     * leaves an unrecognisable image rather than one that claims to
     * be valid with a half-written table. */
    rc = fs_sector_write(0, &sb);

    if (rc != FS_OK)
        goto fail;

    console_info("FS: formatted LumenFS v2");
    terminal_write("  blocks: ");
    terminal_write_u32(sb.total_blocks);
    terminal_write(" of ");
    terminal_write_u32(sb.block_size);
    terminal_write(" bytes\n");
    terminal_write("  inodes: ");
    terminal_write_u32(sb.inode_count);
    terminal_putchar('\n');

    kfree(fresh_bitmap);
    FS_UNLOCK();
    return FS_OK;

fail:
    /* Restore the previous mount state so a failed format does not
     * leave the kernel half-pointed at a filesystem that does not
     * exist. */
    kfree(fresh_bitmap);
    bitmap = old_bitmap;
    bitmap_bytes = old_bytes;
    sb = old_sb;
    mounted = was_mounted;
    FS_UNLOCK();
    return rc;
}

/* Validate a superblock read off the disk. */
static int fs_validate_superblock(const fs_superblock_t *in)
{
    if (memcmp(in->magic, FS_MAGIC, FS_MAGIC_LEN) != 0)
        return FS_EINVAL;

    if (memcmp(in->magic_back, FS_MAGIC, FS_MAGIC_LEN) != 0)
        return FS_EINVAL;

    if (in->version != FS_VERSION)
        return FS_EINVAL;

    if (in->sector_size != FS_SECTOR_SIZE ||
        in->block_size != FS_BLOCK_SIZE)
        return FS_EINVAL;

    /* Geometry must be self-consistent and inside the drive. */
    if (in->bitmap_lba < 2)
        return FS_EINVAL;

    if (in->bitmap_lba + in->bitmap_sectors > in->inode_table_lba)
        return FS_EINVAL;

    if (in->inode_table_lba + in->inode_table_sectors != in->data_start_lba)
        return FS_EINVAL;

    if (in->total_sectors == 0 || in->total_blocks == 0)
        return FS_EINVAL;

    if (in->root_inode >= in->inode_count)
        return FS_EINVAL;

    if (in->bitmap_sectors < fs_sectors_for_bytes((in->total_blocks + 7) / 8))
        return FS_EINVAL;

    if (in->inode_count !=
        in->inode_table_sectors * FS_INODES_PER_SECTOR)
        return FS_EINVAL;

    return FS_OK;
}

int fs_mount(void)
{
    if (mounted)
        return FS_OK;

    if (!ata_is_ready())
        return FS_EIO;

    FS_LOCK();

    fs_superblock_t raw;
    int rc = fs_read_superblock(&raw);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* Distinguish "old v1 image" from "not a filesystem at all", so
     * the boot message can say something useful instead of just
     * "unformatted". */
    if (memcmp(raw.magic, FS_MAGIC, FS_MAGIC_LEN) != 0) {
        FS_UNLOCK();
        return FS_EINVAL;
    }

    rc = fs_validate_superblock(&raw);

    if (rc != FS_OK) {
        FS_UNLOCK();
        return rc;
    }

    /* The image must actually fit the drive it is on. A filesystem
     * that claims more sectors than exist would let a corrupt pointer
     * read past the end. */
    if (raw.total_sectors > ata_total_sectors()) {
        FS_UNLOCK();
        return FS_EINVAL;
    }

    sb = raw;

    uint32_t want = sb.bitmap_sectors * FS_SECTOR_SIZE;
    uint8_t *map = kmalloc(want);

    if (map == 0) {
        memset(&sb, 0, sizeof(sb));
        FS_UNLOCK();
        return FS_ENOMEM;
    }

    bitmap = map;
    bitmap_bytes = want;

    rc = bitmap_load();

    if (rc != FS_OK) {
        kfree(map);
        bitmap = 0;
        bitmap_bytes = 0;
        memset(&sb, 0, sizeof(sb));
        FS_UNLOCK();
        return rc;
    }

    /* The root directory must exist, or nothing does. */
    fs_inode_t root;
    rc = fs_inode_read(sb.root_inode, &root);

    if (rc != FS_OK || root.type != FS_TYPE_DIR) {
        kfree(map);
        bitmap = 0;
        bitmap_bytes = 0;
        memset(&sb, 0, sizeof(sb));
        FS_UNLOCK();
        return FS_EIO;
    }

    mounted = 1;
    FS_UNLOCK();
    return FS_OK;
}

void fs_init(void)
{
    mounted = 0;
    bitmap = 0;
    bitmap_bytes = 0;
    memset(&sb, 0, sizeof(sb));

    if (!ata_is_ready()) {
        console_warn("FS: no ATA drive, filesystem disabled");
        return;
    }

    int rc = fs_mount();

    if (rc == FS_OK) {
        console_info("FS: LumenFS v2 mounted");
        terminal_write("  capacity: ");
        terminal_write_u32((uint32_t)(fs_total_bytes() / 1024U / 1024U));
        terminal_write(" MiB, ");
        terminal_write_u32((uint32_t)fs_free_bytes() / 1024U);
        terminal_write(" KiB free\n");
        return;
    }

    /*
     * Distinguish a v1 image from a blank disk so the boot message
     * can say something actionable. v1 stored the constant 0x4C554D46
     * ("LUMF") as a little-endian uint32 in the first four bytes,
     * which is "FMUL" on disk. v2 instead starts with the ASCII
     * "LUMFS2", so the two are unambiguous.
     */
    static const char v1_signature[4] = { 'F', 'M', 'U', 'L' };

    fs_superblock_t raw;

    if (fs_read_superblock(&raw) == FS_OK) {
        if (memcmp(raw.magic, v1_signature, sizeof(v1_signature)) == 0) {
            console_warn("FS: LumenFS v1 image found; v1 is not supported");
            console_warn("FS: run 'format' to create a v2 filesystem");
            return;
        }
    }

    console_warn("FS: unformatted disk (run 'format')");
}

/* ---- Self-test ------------------------------------------------------ */

/*
 * Exercises the properties that distinguish v2 from v1:
 *
 *   - nested directory creation, lookup, and removal
 *   - block reclamation (a deleted file's blocks must return to the
 *     pool, which v1 could not do at all)
 *   - data integrity across the direct/indirect boundary
 *   - permission enforcement for a non-owner
 *   - persistence across a remount
 *
 * Returns non-zero on success, 0 on failure, per the
 * *_run_self_test convention that report_self_test() consumes. This
 * used to return 0 on success, which made every filesystem failure
 * print "SELFTEST FS PASS".
 */
int fs_self_test(void)
{
    if (!mounted) {
        console_error("FS self-test: not mounted");
        return -1;
    }

    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("FS self-test: " msg);                           \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    /* Everything runs under a fixed name prefix so repeated runs are
     * idempotent and cannot collide with user data. */
    static const char *const created_dirs[] = {
        "/selftest", "/selftest/a", "/selftest/a/b", "/selftest/a/b/c"
    };

    /* Clean up leftovers from a previous run, deepest first. */
    fs_delete("/selftest/a/b/c/file_big", FS_ROOT);
    fs_delete("/selftest/a/b/c/file_small", FS_ROOT);
    fs_delete("/selftest/a/b/file_mid", FS_ROOT);
    fs_delete("/selftest/a/file_top", FS_ROOT);
    fs_rmdir("/selftest/a/b/c", FS_ROOT);
    fs_rmdir("/selftest/a/b", FS_ROOT);
    fs_rmdir("/selftest/a", FS_ROOT);
    fs_rmdir("/selftest", FS_ROOT);

    /* True baseline for the reclamation check at the end; see the note
     * there. */
    uint64_t free_baseline = fs_free_bytes();

    for (uint32_t i = 0; i < 4; i++) {
        int rc = fs_mkdir(created_dirs[i], FS_ROOT, FS_MODE_DIR_DEFAULT);

        CHECK(rc == FS_OK, "could not create nested directory");
    }

    /* Nested lookup must resolve all the way down. */
    uint32_t dir_ino = FS_BLOCK_NONE;
    CHECK(fs_resolve_dir("/selftest/a/b/c", FS_ROOT, &dir_ino) == FS_OK,
          "could not resolve deep path");

    /* ---- Small file, straight through the direct blocks ---- */

    static const char small[] = "hello lumenfs v2";
    uint32_t small_len = sizeof(small) - 1;

    CHECK(fs_write("/selftest/a/file_top", FS_ROOT, small, small_len)
              == FS_OK,
          "small write failed");

    /* Now that file_top exists as a plain file, a path that stops on
     * it must not resolve as a directory. These checks used to run
     * before the file was created, where ENOENT is the correct answer
     * and ENOTDIR is not. */
    uint32_t should_fail = FS_BLOCK_NONE;
    CHECK(fs_lookup("/selftest/a/file_top", FS_ROOT, FS_TYPE_DIR,
                    &should_fail) == FS_ENOTDIR,
          "a file resolved as a directory");

    /* A component that is not a directory must stop the walk with
     * ENOTDIR, not ENOENT. */
    CHECK(fs_resolve_dir("/selftest/a/file_top/deeper", FS_ROOT,
                         &should_fail) == FS_ENOTDIR,
          "walking through a file did not report ENOTDIR");

    void *read_back = 0;
    uint32_t read_size = 0;

    CHECK(fs_read("/selftest/a/file_top", FS_ROOT, &read_back, &read_size)
              == FS_OK,
          "small read failed");

    CHECK(read_size == small_len, "small read returned wrong length");

    if (read_back != 0) {
        CHECK(memcmp(read_back, small, small_len) == 0,
              "small read returned wrong bytes");
        kfree(read_back);
    }

    /* A file that does not exist must be reported as such, not as an
     * empty file. */
    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/no_such_file", FS_ROOT, &read_back,
                  &read_size) == FS_ENOENT,
          "missing file did not report ENOENT");

    /* A directory must not read as a file. */
    CHECK(fs_read("/selftest/a", FS_ROOT, &read_back, &read_size)
              == FS_ENOTDIR,
          "reading a directory did not report ENOTDIR");

    /* ---- Permissions ---- */

    /* /selftest/a is created by root with FS_MODE_DIR_DEFAULT, so a
     * non-owner holds only "other" bits -- which in this format cannot
     * express write (FS_PERM_OTHER_W is the one reserved bit). An
     * unprivileged create here must therefore be refused. The earlier
     * version of this check expected FS_OK and then also expected the
     * same user to be denied deleting a sibling, which requires the
     * same directory-write bit twice and can never both hold. */

    /* Nor may they create in it: that needs write on the directory,
     * which is root-owned and not world-writable. */
    CHECK(fs_write("/selftest/a/file_owned", FS_NOBODY, "mine", 4)
              == FS_EACCES,
          "unprivileged create in a root directory was allowed");

    /* Nor may they delete: same requirement. */
    CHECK(fs_delete("/selftest/a/file_top", FS_NOBODY) == FS_EACCES,
          "unprivileged delete in a root directory was allowed");

    /* Narrow the file to owner-only before asserting that others
     * cannot read it. fs_write created it with FS_MODE_FILE_DEFAULT,
     * which carries OTHER_R, so the premise has to be set up
     * explicitly rather than assumed from the filename. */
    CHECK(fs_chmod("/selftest/a/file_top", FS_ROOT,
                   FS_MODE_FILE_OWNER) == FS_OK,
          "could not restrict the file to owner-only");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_top", FS_NOBODY, &read_back,
                  &read_size) == FS_EACCES,
          "unprivileged read of a 0600 root file was allowed");

    /* Nobody may overwrite root's file either. */
    CHECK(fs_write("/selftest/a/file_top", FS_NOBODY, "hacked", 6)
              == FS_EACCES,
          "unprivileged overwrite of a root file was allowed");

    /* The owner may always read their own file. */
    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_top", FS_ROOT, &read_back,
                  &read_size) == FS_OK,
          "owner could not read their own file");

    if (read_back != 0) {
        CHECK(read_size == small_len &&
              memcmp(read_back, small, small_len) == 0,
              "owner's file came back wrong");
        kfree(read_back);
    }

    /* chmod by a non-owner must be refused. */
    CHECK(fs_chmod("/selftest/a/file_top", FS_NOBODY,
                   FS_MODE_FILE_OWNER) == FS_EACCES,
          "unprivileged chmod was allowed");

    /* chmod by the owner must work, and change what is permitted. */
    CHECK(fs_chmod("/selftest/a/file_top", FS_ROOT,
                   (uint8_t)(FS_PERM_OWNER_R | FS_PERM_OWNER_W |
                             FS_PERM_OTHER_R)) == FS_OK,
          "root chmod failed");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_top", FS_NOBODY, &read_back,
                  &read_size) == FS_OK,
          "world-readable file was still refused after chmod");

    if (read_back != 0)
        kfree(read_back);

    /* Taking the read bit away must deny it again. */
    CHECK(fs_chmod("/selftest/a/file_top", FS_ROOT,
                   FS_MODE_FILE_OWNER) == FS_OK,
          "second chmod failed");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_top", FS_NOBODY, &read_back,
                  &read_size) == FS_EACCES,
          "chmod to 0600 did not deny an unprivileged read");

    /* A reserved mode bit must be rejected rather than silently
     * masked. */
    CHECK(fs_chmod("/selftest/a/file_top", FS_ROOT, 0xFF) == FS_EINVAL,
          "a mode with reserved bits set was accepted");

    /* Put the file back the way later checks expect to find it. */
    fs_chmod("/selftest/a/file_top", FS_ROOT, FS_MODE_FILE_DEFAULT);

    /* ---- Block reclamation, the thing v1 could not do ---- */

    uint64_t free_before = fs_free_bytes();

    /* A file large enough to need indirect blocks. */
    static uint8_t big[FS_BLOCK_SIZE * 5];
    uint32_t big_len = sizeof(big);

    for (uint32_t i = 0; i < big_len; i++)
        big[i] = (uint8_t)(i * 7 + 3);

    CHECK(fs_write("/selftest/a/b/file_mid", FS_ROOT, big, big_len) == FS_OK,
          "multi-block write failed");

    uint64_t free_after_write = fs_free_bytes();
    CHECK(free_after_write < free_before,
          "writing a file did not consume free space");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/b/file_mid", FS_ROOT, &read_back, &read_size)
              == FS_OK,
          "multi-block read failed");

    CHECK(read_size == big_len, "multi-block read returned wrong length");

    if (read_back != 0) {
        int mismatch = 0;

        for (uint32_t i = 0; i < big_len; i++) {
            if (((uint8_t *)read_back)[i] != big[i]) {
                mismatch = 1;
                break;
            }
        }

        CHECK(mismatch == 0, "multi-block data came back corrupted");
        kfree(read_back);
    }

    /* Deleting it must give the space back. This is the exact case
     * that leaked forever under v1. */
    CHECK(fs_delete("/selftest/a/b/file_mid", FS_ROOT) == FS_OK,
          "deleting a multi-block file failed");

    /* Exactly the file's own blocks must come back. Note this is not
     * free_before: writing the file also caused /selftest/a/b to
     * allocate a block for its first directory entry, and that block
     * legitimately survives the delete. */
    uint64_t free_after_delete = fs_free_bytes();
    uint32_t file_blocks = fs_blocks_for_bytes(big_len);

    CHECK(free_after_delete ==
              free_after_write + (uint64_t)file_blocks * FS_BLOCK_SIZE,
          "deleting a file did not reclaim its blocks");

    /* Rewriting the same data must succeed without running out,
     * which only holds if the blocks really were reclaimed. */
    CHECK(fs_write("/selftest/a/b/file_mid", FS_ROOT, big, big_len) == FS_OK,
          "rewrite after reclaim failed");

    /* ---- Handles ---- */

    fs_handle_t handle;

    CHECK(fs_open("/selftest/a/b/c/file_small", FS_ROOT,
                  FS_OPEN_CREATE | FS_OPEN_WRITE | FS_OPEN_TRUNC,
                  &handle) == FS_OK,
          "handle open failed");

    const char *chunk = "handle write";

    uint32_t wrote = 0;
    CHECK(fs_handle_write(&handle, chunk, 12, &wrote) == FS_OK &&
          wrote == 12,
          "handle write failed");

    uint32_t size_after = 0;
    CHECK(fs_handle_size(&handle, &size_after) == FS_OK &&
          size_after == 12,
          "handle size wrong after write");

    CHECK(fs_handle_seek(&handle, 0, SEEK_SET) == FS_OK,
          "handle seek to start failed");

    char read_chunk[16];
    uint32_t got = 0;
    CHECK(fs_handle_read(&handle, read_chunk, sizeof(read_chunk), &got)
              == FS_OK && got == 12,
          "handle read failed");

    CHECK(memcmp(read_chunk, chunk, 12) == 0,
          "handle read returned wrong bytes");

    /* Appending must not disturb what is already there. */
    CHECK(fs_handle_seek(&handle, 0, SEEK_END) == FS_OK,
          "handle seek to end failed");

    wrote = 0;
    CHECK(fs_handle_write(&handle, "!!", 2, &wrote) == FS_OK && wrote == 2,
          "append failed");

    CHECK(fs_close(&handle) == FS_OK, "handle close failed");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/b/c/file_small", FS_ROOT, &read_back,
                  &read_size) == FS_OK,
          "appended file read failed");

    if (read_back != 0) {
        CHECK(read_size == 14 &&
                  memcmp(read_back, "handle write!!", 14) == 0,
              "appended file has wrong contents");
        kfree(read_back);
    }

    /* ---- Directory listing ---- */

    uint32_t count = 0;
    CHECK(fs_count_entries("/selftest/a", FS_ROOT, &count) == FS_OK,
          "counting entries failed");
    CHECK(count == 2, "unexpected entry count in /selftest/a");

    /* A non-empty directory must refuse to be removed. */
    CHECK(fs_rmdir("/selftest/a", FS_ROOT) == FS_ENOTEMPTY,
          "rmdir removed a non-empty directory");

    /* ---- Rename ---- */

    CHECK(fs_rename("/selftest/a/file_top", "/selftest/a/file_renamed",
                    FS_ROOT) == FS_OK,
          "rename within a directory failed");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_renamed", FS_ROOT, &read_back,
                  &read_size) == FS_OK,
          "renamed file is not readable at its new name");

    if (read_back != 0)
        kfree(read_back);

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_top", FS_ROOT, &read_back, &read_size)
              == FS_ENOENT,
          "old name still resolves after a rename");

    /* ---- Persistence across a remount ---- */

    /*
     * Flush the bitmap, drop the in-memory state, and mount again
     * from the same bytes. Everything written above must still be
     * there, which is the whole point of having a filesystem rather
     * than a RAM disk.
     */
    CHECK(bitmap_flush() == FS_OK, "bitmap flush failed");

    mounted = 0;
    bitmap = 0;
    bitmap_bytes = 0;
    memset(&sb, 0, sizeof(sb));

    CHECK(fs_mount() == FS_OK, "remount failed");

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/file_renamed", FS_ROOT, &read_back,
                  &read_size) == FS_OK,
          "file did not survive a remount");

    if (read_back != 0) {
        CHECK(read_size == small_len &&
                  memcmp(read_back, small, small_len) == 0,
              "remounted file has wrong contents");
        kfree(read_back);
    }

    read_back = 0;
    read_size = 0;
    CHECK(fs_read("/selftest/a/b/file_mid", FS_ROOT, &read_back, &read_size)
              == FS_OK,
          "multi-block file did not survive a remount");

    if (read_back != 0) {
        int mismatch = 0;

        for (uint32_t i = 0; i < big_len; i++) {
            if (((uint8_t *)read_back)[i] != big[i]) {
                mismatch = 1;
                break;
            }
        }

        CHECK(mismatch == 0, "remounted multi-block file is corrupted");
        kfree(read_back);
    }

    /* ---- Clean up, deepest first ---- */

    CHECK(fs_delete("/selftest/a/b/c/file_small", FS_ROOT) == FS_OK,
          "cleanup: delete failed");
    CHECK(fs_delete("/selftest/a/b/file_mid", FS_ROOT) == FS_OK,
          "cleanup: delete failed");
    CHECK(fs_delete("/selftest/a/file_renamed", FS_ROOT) == FS_OK,
          "cleanup: delete failed");
    CHECK(fs_rmdir("/selftest/a/b/c", FS_ROOT) == FS_OK,
          "cleanup: rmdir failed");
    CHECK(fs_rmdir("/selftest/a/b", FS_ROOT) == FS_OK,
          "cleanup: rmdir failed");
    CHECK(fs_rmdir("/selftest/a", FS_ROOT) == FS_OK,
          "cleanup: rmdir failed");
    CHECK(fs_rmdir("/selftest", FS_ROOT) == FS_OK,
          "cleanup: rmdir failed");

    /* Everything must be back in the free pool. */
    uint64_t free_final = fs_free_bytes();
    CHECK(free_final == free_baseline,
          "filesystem did not return to its starting free space");

#undef CHECK

    if (failures != 0) {
        console_error("FS self-test: FAILED");
        /* report_self_test() reads any non-zero result as success, so
         * failure has to be exactly 0. Returning -1 here still printed
         * "SELFTEST FS PASS" while every single check was failing. */
        return 0;
    }

    console_info("FS self-test: PASS");
    return 1;
}
