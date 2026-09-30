/*
 * tools/mkfs.c — host-side LumenFS v2 formatter.
 *
 * Produces an image identical to what the kernel's fs_format()
 * writes: same superblock layout (fs_format.h), same bitmap with
 * the metadata blocks reserved, a zeroed inode table, a root
 * directory inode, and the superblock written last.
 *
 * Build (wired into the Makefile):  make tools/mkfs
 * Use:  ./tools/mkfs <image> <size-sectors> [uid] [gid]
 *
 * This file uses only fs_format.h and the C library, so it
 * compiles with the native host compiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../kernel/fs_format.h"

static int plan_layout(uint32_t total_sectors, fs_superblock_t *out)
{
    if (out == 0)
        return -1;

    if (total_sectors < fs_minimum_sectors())
        return -1;

    uint32_t total_blocks = total_sectors / FS_BLOCK_SECTORS;

    if (total_blocks < 16)
        return -1;

    memset(out, 0, sizeof(*out));

    memcpy(out->magic, FS_MAGIC, FS_MAGIC_LEN);
    out->version = FS_VERSION;
    out->sector_size = FS_SECTOR_SIZE;
    out->block_size = FS_BLOCK_SIZE;
    out->total_sectors = total_sectors;
    out->total_blocks = total_blocks;

    out->bitmap_sectors =
        fs_sectors_for_bytes((total_blocks + 7) / 8);
    out->bitmap_lba = 2;

    uint32_t min_inode_sectors =
        fs_sectors_for_bytes(128 * FS_INODE_SIZE);
    uint32_t inode_sectors = 1;

    while (inode_sectors < min_inode_sectors)
        inode_sectors *= 2;

    out->inode_table_lba = out->bitmap_lba + out->bitmap_sectors;

    /* Grow (never shrink) the inode table so data_start_lba is
     * block-aligned while inode_table_lba + sectors ==
     * data_start_lba exactly, mirroring the kernel. */
    while ((out->inode_table_lba + inode_sectors) %
           FS_BLOCK_SECTORS != 0)
        inode_sectors++;

    out->inode_table_sectors = inode_sectors;
    out->inode_count = inode_sectors * FS_INODES_PER_SECTOR;

    out->data_start_lba =
        out->inode_table_lba + out->inode_table_sectors;
    out->root_inode = 0;

    if (out->data_start_lba >= total_sectors)
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5) {
        fprintf(stderr,
                "usage: %s <image> <size-sectors> [uid] [gid]\n",
                argv[0]);
        return 1;
    }

    const char *path = argv[1];
    uint32_t total_sectors = (uint32_t)strtoul(argv[2], 0, 0);
    uint16_t uid = argc > 3 ? (uint16_t)strtoul(argv[3], 0, 0) : 0;
    uint16_t gid = argc > 4 ? (uint16_t)strtoul(argv[4], 0, 0) : 0;

    fs_superblock_t sb;

    if (plan_layout(total_sectors, &sb) != 0) {
        fprintf(stderr, "mkfs: drive too small to format\n");
        return 1;
    }

    uint32_t needed = sb.data_start_lba + FS_BLOCK_SECTORS;

    if (needed > total_sectors) {
        fprintf(stderr, "mkfs: drive too small to format\n");
        return 1;
    }

    FILE *f = fopen(path, "wb+");

    if (!f) {
        perror("mkfs: fopen");
        return 1;
    }

    /* Pre-size the image. */
    if (fseek(f, (long)(total_sectors * FS_SECTOR_SIZE) - 1,
              SEEK_SET) != 0 ||
        fputc(0, f) == EOF) {
        perror("mkfs: sizing image");
        fclose(f);
        return 1;
    }

    fflush(f);

    uint8_t sector[FS_SECTOR_SIZE];

    /* Bitmap with the metadata blocks reserved, mirroring
     * bitmap_reserve_metadata(): blocks [bitmap_lba/2,
     * ceil(data_start_lba/2)) are in use. */
    memset(sector, 0, sizeof(sector));

    uint32_t first_meta = sb.bitmap_lba / FS_BLOCK_SECTORS;
    uint32_t last_meta =
        (sb.data_start_lba + FS_BLOCK_SECTORS - 1) /
        FS_BLOCK_SECTORS;
    uint8_t *bitmap =
        calloc(sb.bitmap_sectors * FS_SECTOR_SIZE, 1);

    if (!bitmap) {
        fprintf(stderr, "mkfs: out of memory\n");
        fclose(f);
        return 1;
    }

    for (uint32_t b = first_meta; b < last_meta; b++) {
        if (b < sb.total_blocks)
            bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
    }

    for (uint32_t i = 0; i < sb.bitmap_sectors; i++) {
        fseek(f, (long)(sb.bitmap_lba + i) * FS_SECTOR_SIZE,
              SEEK_SET);

        if (fwrite(bitmap + i * FS_SECTOR_SIZE, 1,
                   FS_SECTOR_SIZE, f) != FS_SECTOR_SIZE) {
            perror("mkfs: writing bitmap");
            free(bitmap);
            fclose(f);
            return 1;
        }
    }

    free(bitmap);

    /* Blank the inode table. */
    memset(sector, 0, sizeof(sector));

    for (uint32_t i = 0; i < sb.inode_table_sectors; i++) {
        fseek(f,
              (long)(sb.inode_table_lba + i) * FS_SECTOR_SIZE,
              SEEK_SET);

        if (fwrite(sector, 1, FS_SECTOR_SIZE, f) !=
            FS_SECTOR_SIZE) {
            perror("mkfs: writing inode table");
            fclose(f);
            return 1;
        }
    }

    /* Root directory inode, mirroring fs_inode_init() with
     * FS_TYPE_DIR / FS_MODE_DIR_DEFAULT. */
    {
        uint32_t now = (uint32_t)time(0);
        fs_inode_t root;

        memset(&root, 0, sizeof(root));
        root.type = FS_TYPE_DIR;
        root.mode =
            (uint8_t)(FS_MODE_DIR_DEFAULT & FS_MODE_MASK);
        root.uid = uid;
        root.gid = gid;
        root.size = 0;
        root.mtime = now;
        root.flags = 0;

        for (int i = 0; i < FS_DIRECT_BLOCKS; i++)
            root.direct[i] = FS_BLOCK_NONE;

        root.indirect = FS_BLOCK_NONE;

        fseek(f, (long)sb.inode_table_lba * FS_SECTOR_SIZE,
              SEEK_SET);

        /* Inode 0 is at offset 0 of the table. The whole root
         * struct is stored, matching the kernel's slot-0
         * read exactly. */
        uint8_t first[FS_SECTOR_SIZE];
        memset(first, 0, sizeof(first));
        memcpy(first, &root, sizeof(root));

        if (fwrite(first, 1, FS_SECTOR_SIZE, f) !=
            FS_SECTOR_SIZE) {
            perror("mkfs: writing root inode");
            fclose(f);
            return 1;
        }
    }

    /* Stamps mirror fs_format(): epoch ^ sectors and a fixed
     * multiplicative hash of the data start. */
    sb.uuid_lo = (uint32_t)time(0) ^ total_sectors;
    sb.uuid_hi = sb.data_start_lba * 2654435761U;
    memcpy(sb.magic_back, FS_MAGIC, FS_MAGIC_LEN);
    sb.format_version_check = FS_VERSION;

    /* Superblock last, same crash-ordering rationale as the
     * kernel formatter. */
    fseek(f, 0, SEEK_SET);

    uint8_t sblk[FS_SECTOR_SIZE];
    memset(sblk, 0, sizeof(sblk));
    memcpy(sblk, &sb, sizeof(sb));

    if (fwrite(sblk, 1, FS_SECTOR_SIZE, f) != FS_SECTOR_SIZE) {
        perror("mkfs: writing superblock");
        fclose(f);
        return 1;
    }

    fclose(f);

    printf("mkfs: %s: LumenFS v2, %u sectors, %u blocks, %u inodes\n",
           path, sb.total_sectors, sb.total_blocks,
           sb.inode_count);
    return 0;
}
