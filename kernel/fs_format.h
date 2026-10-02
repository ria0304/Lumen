#ifndef FS_FORMAT_H
#define FS_FORMAT_H

/*
 * LumenFS v2 on-disk format.
 *
 * Shared verbatim between the kernel (kernel/fs.c) and the host-side
 * image builder (tools/mkfs.c). It must stay free of kernel-specific
 * types so a native x86-64 compiler can include it unchanged.
 *
 * Disk geometry, in 512-byte sectors:
 *
 *   LBA 0                      superblock
 *   LBA 1                      reserved (a spare superblock copy lands
 *                              here eventually; kept out of the pool so
 *                              the geometry does not have to change)
 *   bitmap_lba .. +sectors     block allocation bitmap, 1 bit per block
 *   inode_table_lba .. +sect   inode table
 *   data_start_lba ..          file and directory data blocks
 *
 * Allocations are tracked in units of blocks, not sectors, so a block
 * is FS_BLOCK_SECTORS sectors wide and two bitmap bits cover one
 * sector pair.
 *
 * This format is NOT compatible with LumenFS v1. v1 images are
 * rejected at mount rather than silently misread, because the two
 * layouts share no structure beyond "some bytes on sector 0".
 */

#include <stdint.h>

#define FS_MAGIC        "LUMFS2"
#define FS_MAGIC_LEN    6
#define FS_VERSION      2
#define FS_SECTOR_SIZE  512

/*
 * A block is 1 KiB: two sectors. One sector is too small to amortise
 * per-block bookkeeping, and the 4 MiB minimum image still leaves
 * plenty of blocks at this size.
 */
#define FS_BLOCK_SIZE     1024
#define FS_BLOCK_SECTORS  (FS_BLOCK_SIZE / FS_SECTOR_SIZE)
#define FS_BLOCKS_PER_SECTOR (FS_SECTOR_SIZE * 8)

/* Direct pointers in an inode, before the single indirect block. */
#define FS_DIRECT_BLOCKS 10

/* Pointers addressable through one indirect block. */
#define FS_INDIRECT_PTRS  ((FS_BLOCK_SIZE / 4) - 1)

/* Maximum file size: direct blocks plus one indirect block. */
#define FS_MAX_FILE_SIZE \
    ((FS_DIRECT_BLOCKS + FS_INDIRECT_PTRS) * FS_BLOCK_SIZE)

#define FS_INODE_SIZE     64
#define FS_INODES_PER_SECTOR (FS_SECTOR_SIZE / FS_INODE_SIZE)

/* Directory entry size, padded so entries never straddle a sector. */
#define FS_DIRENT_SIZE    32

/*
 * Longest name storable in a directory entry, NUL included.
 *
 * Sized so fs_dirent_t is exactly FS_DIRENT_SIZE: a u32 plus two u8
 * is 6 bytes, leaving 26 for the name. It used to be 28, which made
 * the struct 36 bytes while entries are stored 32 at a time, so the
 * tail of every long name was silently dropped on write and read back
 * as stack garbage.
 */
#define FS_NAME_MAX       26

#define FS_PATH_MAX       256

/* File types, stored in inode.type. 0 means the inode is free. */
#define FS_TYPE_FREE      0
#define FS_TYPE_FILE      1
#define FS_TYPE_DIR       2
#define FS_TYPE_SYMLINK   3

/*
 * Permission bits, in the usual Unix arrangement. Stored as one
 * byte; FS_PERM_OTHER_W is the single reserved bit, which must be
 * zero. Every other bit is a live permission, including
 * FS_PERM_OTHER_R -- a narrower mask silently drops world-read
 * from FS_MODE_DIR_DEFAULT.
 */
#define FS_PERM_OWNER_R  0x01
#define FS_PERM_OWNER_W  0x02
#define FS_PERM_OWNER_X  0x04
#define FS_PERM_GROUP_R  0x08
#define FS_PERM_GROUP_W  0x10
#define FS_PERM_GROUP_X  0x20
#define FS_PERM_OTHER_R  0x40
#define FS_PERM_OTHER_W  0x80

/* All defined permission bits except the reserved OTHER_W. */
#define FS_MODE_MASK     0x7F

/* Convenience for a file only its owner may touch. */
#define FS_MODE_FILE_OWNER (FS_PERM_OWNER_R | FS_PERM_OWNER_W)
#define FS_MODE_DIR_OWNER  (FS_PERM_OWNER_R | \
                            FS_PERM_OWNER_W | \
                            FS_PERM_OWNER_X)
#define FS_MODE_DIR_DEFAULT (FS_PERM_OWNER_R | FS_PERM_OWNER_W | \
                             FS_PERM_OWNER_X | FS_PERM_GROUP_R | \
                             FS_PERM_GROUP_X | FS_PERM_OTHER_R)
#define FS_MODE_FILE_DEFAULT (FS_PERM_OWNER_R | FS_PERM_OWNER_W | \
                              FS_PERM_GROUP_R | FS_PERM_OTHER_R)

/* Sentinel stored in a block pointer meaning "no block". */
#define FS_BLOCK_NONE    0xFFFFFFFFu

/* The superblock. Padded to exactly one sector. */
typedef struct {
    char     magic[8];            /* FS_MAGIC, NUL padded */
    uint32_t version;             /* FS_VERSION */
    uint32_t sector_size;         /* always FS_SECTOR_SIZE */
    uint32_t block_size;          /* always FS_BLOCK_SIZE */
    uint32_t total_sectors;       /* size of the whole drive */
    uint32_t total_blocks;        /* total_sectors / FS_BLOCK_SECTORS */
    uint32_t bitmap_lba;          /* first sector of the block bitmap */
    uint32_t bitmap_sectors;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t inode_count;         /* usable inodes (table size) */
    uint32_t data_start_lba;      /* first sector available for data */
    uint32_t root_inode;          /* always 0 in practice */
    uint32_t uuid_lo;             /* filled from the RTC at format time */
    uint32_t uuid_hi;
    char     magic_back[8];       /* duplicate of the leading magic */
    uint32_t format_version_check;
    uint8_t  reserved[FS_SECTOR_SIZE - 76];
} fs_superblock_t;

/*
 * An inode. Exactly FS_INODE_SIZE bytes so a whole number fits in
 * every sector.
 *
 * The layout is deliberate and sums to precisely 64 bytes: two bytes
 * of u8, two u16, then 2 bytes of alignment padding before the first
 * u32, three u32 scalars, FS_DIRECT_BLOCKS pointers and the indirect
 * pointer. 8 + 12 + 40 + 4 == 64.
 *
 * There used to be a reserved[8] here, which pushed sizeof() to 72
 * while FS_INODE_SIZE still said 64. FS_INODES_PER_SECTOR was then 8
 * when only 7 actually fit in a 512-byte sector, so writing the
 * eighth inode in a sector ran 64 bytes past the caller's buffer and
 * smashed the stack. The assertions below fail the build if the
 * struct and the declared size ever disagree again.
 */
typedef struct {
    uint8_t  type;                /* FS_TYPE_* */
    uint8_t  mode;                /* FS_PERM_* */
    uint16_t uid;
    uint16_t gid;
    /* 2 bytes of alignment padding here, ahead of 'size'. */
    uint32_t size;                /* bytes, for files */
    uint32_t mtime;               /* Unix epoch seconds */
    uint32_t flags;               /* reserved, must be 0 */
    uint32_t direct[FS_DIRECT_BLOCKS];
    uint32_t indirect;            /* FS_BLOCK_NONE when unused */
} fs_inode_t;

_Static_assert(sizeof(fs_inode_t) == FS_INODE_SIZE,
               "fs_inode_t must be exactly FS_INODE_SIZE bytes");
_Static_assert(FS_SECTOR_SIZE % FS_INODE_SIZE == 0,
               "FS_INODE_SIZE must divide a whole number of sectors");

/*
 * A directory entry. Directories are arrays of these; nothing else
 * describes their contents, so the entry count is derived from the
 * directory inode's size.
 *
 * ino is FS_BLOCK_NONE for an entry whose inode has since been freed,
 * which is how a name is retired without compacting the array.
 */
typedef struct {
    uint32_t ino;                 /* inode number, or FS_BLOCK_NONE */
    uint8_t  type;                /* FS_TYPE_FILE / FS_TYPE_DIR */
    uint8_t  name_len;            /* bytes in name, excluding NUL */
    char     name[FS_NAME_MAX];   /* NUL terminated */
} fs_dirent_t;

_Static_assert(sizeof(fs_dirent_t) == FS_DIRENT_SIZE,
               "fs_dirent_t must be exactly FS_DIRENT_SIZE bytes");
_Static_assert(FS_SECTOR_SIZE % FS_DIRENT_SIZE == 0,
               "FS_DIRENT_SIZE must divide a whole number of sectors");
_Static_assert(sizeof(fs_superblock_t) == FS_SECTOR_SIZE,
               "fs_superblock_t must be exactly one sector");

/* ---- Shared geometry helpers --------------------------------------- */

/* Blocks needed to hold a byte count, rounding up. */
static inline uint32_t fs_blocks_for_bytes(uint32_t bytes)
{
    return (bytes + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
}

/* Sectors needed to hold a byte count, rounding up. */
static inline uint32_t fs_sectors_for_bytes(uint32_t bytes)
{
    return (bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
}

/*
 * Minimum sectors a filesystem of the given size needs for its
 * metadata. A format that cannot fit this much is refused rather than
 * writing a superblock that describes an impossible layout.
 */
static inline uint32_t fs_minimum_sectors(void)
{
    /* superblock + reserved + a bitmap sector + an inode table with
     * some slack + one data block. */
    return 2 + 1 + 8 + FS_BLOCK_SECTORS;
}

#endif
