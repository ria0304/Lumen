#include <stdint.h>
#include "fs.h"
#include "ata.h"
#include "console.h"

/*
 * On-disk layout is serialized by hand, byte by byte -- no struct
 * is ever read or written directly, so there's no dependency on
 * this compiler's packing/endianness. See fs.h for the layout.
 */
#define FS_ENTRY_WIRE_SIZE (FS_NAME_LEN + 4 + 4 + 1) /* 37 bytes */

static fs_entry_t directory[FS_MAX_FILES];
static uint32_t fs_next_free_lba = FS_DATA_START_LBA;
static uint32_t fs_mounted = 0;

/* Scratch buffers for (de)serializing the superblock/directory.
 * Static, not stack-allocated: fs_flush()/fs_init() may run from
 * a task with only a 4 KiB stack, and FS_DIR_SECTORS * 512 bytes
 * would not fit there. */
static uint8_t fs_sector_scratch[512];
static uint8_t fs_dir_scratch[FS_DIR_SECTORS * 512];

static void put_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static uint32_t fs_string_length(const char *text, uint32_t max)
{
    uint32_t length = 0;

    while (length < max && text[length] != '\0')
        length++;

    return length;
}

static int fs_string_equal(const char *a, const char *b)
{
    uint32_t i = 0;

    while (i < FS_NAME_LEN) {

        if (a[i] != b[i])
            return 0;

        if (a[i] == '\0')
            return 1;

        i++;
    }

    return 1;
}

static void fs_name_copy(char *dst, const char *src)
{
    uint32_t length = fs_string_length(src, FS_NAME_LEN - 1);
    uint32_t i;

    for (i = 0; i < length; i++)
        dst[i] = src[i];

    for (; i < FS_NAME_LEN; i++)
        dst[i] = '\0';
}

static uint32_t fs_sectors_for_size(uint32_t size)
{
    return (size + 511U) / 512U;
}

/* Writes the current in-memory superblock + directory to disk.
 * Called after every create/write/delete so a reboot never loses
 * a change that already returned success to the caller. */
static int fs_flush(void)
{
    if (!ata_is_ready())
        return -1;

    for (int i = 0; i < 512; i++)
        fs_sector_scratch[i] = 0;

    put_u32(fs_sector_scratch + 0, FS_MAGIC);
    put_u32(fs_sector_scratch + 4, fs_next_free_lba);

    if (ata_write_sectors(0, 1, fs_sector_scratch) != 0)
        return -1;

    for (uint32_t i = 0; i < sizeof(fs_dir_scratch); i++)
        fs_dir_scratch[i] = 0;

    uint32_t offset = 0;

    for (int i = 0; i < FS_MAX_FILES; i++) {

        for (int j = 0; j < FS_NAME_LEN; j++)
            fs_dir_scratch[offset + (uint32_t)j] =
                (uint8_t)directory[i].name[j];

        offset += FS_NAME_LEN;

        put_u32(fs_dir_scratch + offset, directory[i].size_bytes);
        offset += 4;

        put_u32(fs_dir_scratch + offset, directory[i].start_lba);
        offset += 4;

        fs_dir_scratch[offset] = directory[i].used;
        offset += 1;
    }

    if (ata_write_sectors(
            FS_DIR_START_LBA,
            (uint8_t)FS_DIR_SECTORS,
            fs_dir_scratch) != 0)
        return -1;

    return 0;
}

void fs_init(void)
{
    fs_mounted = 0;

    if (!ata_is_ready()) {
        console_warn("FS: no disk, filesystem unavailable");
        return;
    }

    if (ata_read_sectors(0, 1, fs_sector_scratch) != 0) {
        console_warn("FS: superblock read failed");
        return;
    }

    uint32_t magic = get_u32(fs_sector_scratch + 0);

    if (magic != FS_MAGIC) {
        console_warn("FS: unformatted disk (run 'format')");
        return;
    }

    fs_next_free_lba = get_u32(fs_sector_scratch + 4);

    if (ata_read_sectors(
            FS_DIR_START_LBA,
            (uint8_t)FS_DIR_SECTORS,
            fs_dir_scratch) != 0) {
        console_warn("FS: directory read failed");
        return;
    }

    uint32_t offset = 0;

    for (int i = 0; i < FS_MAX_FILES; i++) {

        for (int j = 0; j < FS_NAME_LEN; j++)
            directory[i].name[j] =
                (char)fs_dir_scratch[offset + (uint32_t)j];

        offset += FS_NAME_LEN;

        directory[i].size_bytes = get_u32(fs_dir_scratch + offset);
        offset += 4;

        directory[i].start_lba = get_u32(fs_dir_scratch + offset);
        offset += 4;

        directory[i].used = fs_dir_scratch[offset];
        offset += 1;
    }

    fs_mounted = 1;
    console_info("FS: LumenFS mounted");
}

int fs_is_mounted(void)
{
    return (int)fs_mounted;
}

int fs_format(void)
{
    if (!ata_is_ready()) {
        console_error("FS: no disk, cannot format");
        return -1;
    }

    for (int i = 0; i < FS_MAX_FILES; i++) {

        for (int j = 0; j < FS_NAME_LEN; j++)
            directory[i].name[j] = '\0';

        directory[i].size_bytes = 0;
        directory[i].start_lba = 0;
        directory[i].used = 0;
    }

    fs_next_free_lba = FS_DATA_START_LBA;
    fs_mounted = 1;

    if (fs_flush() != 0) {
        fs_mounted = 0;
        console_error("FS: format failed");
        return -1;
    }

    console_info("FS: formatted");
    return 0;
}

static int fs_find(const char *name)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {

        if (directory[i].used && fs_string_equal(directory[i].name, name))
            return i;
    }

    return -1;
}

static int fs_find_free_slot(void)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {

        if (!directory[i].used)
            return i;
    }

    return -1;
}

int fs_write(const char *name, const void *data, uint32_t size)
{
    if (!fs_mounted || size == 0)
        return -1;

    uint32_t sectors_needed = fs_sectors_for_size(size);

    if (sectors_needed > FS_MAX_FILE_SECTORS)
        return -1;

    int idx = fs_find(name);
    uint32_t start_lba;

    if (idx >= 0) {

        uint32_t old_sectors = fs_sectors_for_size(directory[idx].size_bytes);

        if (old_sectors > 0 && sectors_needed <= old_sectors) {
            start_lba = directory[idx].start_lba;
        } else {
            start_lba = fs_next_free_lba;
            fs_next_free_lba += sectors_needed;
        }

    } else {

        idx = fs_find_free_slot();

        if (idx < 0)
            return -1;

        start_lba = fs_next_free_lba;
        fs_next_free_lba += sectors_needed;
    }

    const uint8_t *src = (const uint8_t *)data;

    for (uint32_t s = 0; s < sectors_needed; s++) {

        uint32_t offset = s * 512U;
        uint32_t remaining = size - offset;
        uint32_t n = (remaining < 512U) ? remaining : 512U;

        for (uint32_t i = 0; i < n; i++)
            fs_sector_scratch[i] = src[offset + i];

        for (uint32_t i = n; i < 512U; i++)
            fs_sector_scratch[i] = 0;

        if (ata_write_sectors(start_lba + s, 1, fs_sector_scratch) != 0)
            return -1;
    }

    fs_name_copy(directory[idx].name, name);
    directory[idx].size_bytes = size;
    directory[idx].start_lba = start_lba;
    directory[idx].used = 1;

    return fs_flush();
}

int fs_read(const char *name, void *buffer, uint32_t buffer_size)
{
    if (!fs_mounted)
        return -1;

    int idx = fs_find(name);

    if (idx < 0)
        return -1;

    uint32_t total = directory[idx].size_bytes;
    uint32_t to_read = (total < buffer_size) ? total : buffer_size;
    uint32_t start_lba = directory[idx].start_lba;
    uint8_t *dst = (uint8_t *)buffer;

    uint32_t sectors = fs_sectors_for_size(to_read);

    for (uint32_t s = 0; s < sectors; s++) {

        if (ata_read_sectors(start_lba + s, 1, fs_sector_scratch) != 0)
            return -1;

        uint32_t offset = s * 512U;
        uint32_t remaining = to_read - offset;
        uint32_t n = (remaining < 512U) ? remaining : 512U;

        for (uint32_t i = 0; i < n; i++)
            dst[offset + i] = fs_sector_scratch[i];
    }

    return (int)to_read;
}

int fs_delete(const char *name)
{
    if (!fs_mounted)
        return -1;

    int idx = fs_find(name);

    if (idx < 0)
        return -1;

    directory[idx].used = 0;
    directory[idx].size_bytes = 0;
    directory[idx].name[0] = '\0';
    /* start_lba is left as-is: its space is simply never reused,
     * same tradeoff as kernel/heap.c's original bump allocator. */

    return fs_flush();
}

const fs_entry_t *fs_stat(const char *name)
{
    if (!fs_mounted)
        return 0;

    int idx = fs_find(name);

    if (idx < 0)
        return 0;

    return &directory[idx];
}

static void fs_print_uint(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (count > 0)
        terminal_putchar(digits[--count]);
}

void fs_list(void)
{
    if (!fs_mounted) {
        console_warn("FS: not mounted");
        return;
    }

    int any = 0;

    for (int i = 0; i < FS_MAX_FILES; i++) {

        if (!directory[i].used)
            continue;

        any = 1;
        terminal_write(directory[i].name);
        terminal_write("  ");
        fs_print_uint(directory[i].size_bytes);
        terminal_write(" bytes\n");
    }

    if (!any)
        terminal_write("(no files)\n");
}

uint32_t fs_file_count(void)
{
    uint32_t count = 0;

    for (int i = 0; i < FS_MAX_FILES; i++) {

        if (directory[i].used)
            count++;
    }

    return count;
}
