#include <stdint.h>
#include <stddef.h>
#include "ata.h"
#include "console.h"

/* Primary ATA bus, I/O port block. */
#define ATA_IO_BASE     0x1F0
#define ATA_DATA        (ATA_IO_BASE + 0)
#define ATA_ERROR       (ATA_IO_BASE + 1)
#define ATA_SECCOUNT    (ATA_IO_BASE + 2)
#define ATA_LBA_LOW     (ATA_IO_BASE + 3)
#define ATA_LBA_MID     (ATA_IO_BASE + 4)
#define ATA_LBA_HIGH    (ATA_IO_BASE + 5)
#define ATA_DRIVE_HEAD  (ATA_IO_BASE + 6)
#define ATA_STATUS      (ATA_IO_BASE + 7)
#define ATA_COMMAND     (ATA_IO_BASE + 7)

/* Control block: the "alternate status" register, separate from
 * ATA_STATUS so reading it never clears a pending IRQ (moot here
 * since we poll, but it's also the only safe way to read status
 * right after selecting a drive, before its own status is
 * guaranteed valid). */
#define ATA_CONTROL     0x3F6

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH 0xE7

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_SRV  0x10
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_BSY  0x80

static int drive_ready = 0;

/* Drive capacity and identity, filled from the IDENTIFY payload. */
static uint64_t total_sectors = 0;
static char model[41];
static char serial[21];
static int needs_lba48 = 0;

/*
 * IDENTIFY word indices (the payload is 256 16-bit words; words are
 * 1-based in the spec, 0-based here).
 */
#define IDENTIFY_WORDS           256
#define IDENTIFY_SERIAL_W0       10   /* words 10-19, byte-swapped */
#define IDENTIFY_MODEL_W0        27   /* words 27-46, byte-swapped */
#define IDENTIFY_LBA28_TOTAL_W0  60   /* words 60-61 */
#define IDENTIFY_LBA28_TOTAL_W1  61
#define IDENTIFY_LBA48_TOTAL_W0  100  /* words 100-103 */
#define IDENTIFY_LBA48_TOTAL_W1  101
#define IDENTIFY_LBA48_TOTAL_W2  102
#define IDENTIFY_LBA48_TOTAL_W3  103
#define IDENTIFY_LBA48_VALID     0x4000 /* bit 14 of word 49 */

#define LBA28_MAX_SECTORS 0x0FFFFFFFull

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Tiny fixed-count busy-wait, used after drive-select before the
 * status register is trustworthy. Not calibrated to real time --
 * just enough I/O port reads to burn the ~400ns the spec wants. */
static void ata_io_delay(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_CONTROL);
}

static int ata_wait_not_busy(void)
{
    /* Bounded spin: a real disk clears BSY in microseconds. If it
     * never does, there's no drive (or QEMU gave us none), so give
     * up rather than hang the kernel forever. */
    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        if ((inb(ATA_STATUS) & ATA_STATUS_BSY) == 0)
            return 0;
    }
    return -1;
}

static int ata_wait_drq(void)
{
    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        uint8_t status = inb(ATA_STATUS);

        if (status & ATA_STATUS_ERR)
            return -1;

        if (status & ATA_STATUS_DF)
            return -1;

        if (status & ATA_STATUS_DRQ)
            return 0;
    }
    return -1;
}

/* Copy a fixed-width, space-padded, byte-swapped IDENTIFY string
 * field into a NUL-terminated C string. The spec says the bytes in
 * each 16-bit word are stored high-byte-first, which is the reverse
 * of how a word read from the data port is laid out in memory. */
static void ata_copy_identify_string(char *dest, size_t dest_size,
                                     const uint16_t *words, int word_count)
{
    size_t out = 0;

    for (int i = 0; i < word_count; i++) {
        uint16_t w = words[i];

        for (int b = 1; b >= 0; b--) {
            char c = (char)((w >> (b * 8)) & 0xFF);

            /* Trailing spaces are padding, not part of the name. */
            if (c == ' ' || c == '\0')
                continue;

            if (out + 1 < dest_size)
                dest[out++] = c;
        }
    }

    if (dest_size > 0)
        dest[out] = '\0';
}

/*
 * Decode the 256-word IDENTIFY payload. The words are consumed by
 * polling inw(), so the whole thing has to be read regardless; the
 * point here is to keep the fields we actually need.
 */
static void ata_parse_identify(const uint16_t *id)
{
    uint32_t lba28 = (uint32_t)id[IDENTIFY_LBA28_TOTAL_W0] |
                     ((uint32_t)id[IDENTIFY_LBA28_TOTAL_W1] << 16);

    total_sectors = lba28;
    needs_lba48 = 0;

    /*
     * Word 49 bit 14 means the 48-bit total in words 100-103 is
     * valid, and it counts the real capacity rather than whatever
     * the legacy 28-bit fields were clipped to. Prefer it.
     */
    if (id[49] & IDENTIFY_LBA48_VALID) {
        uint64_t lba48 = (uint64_t)id[IDENTIFY_LBA48_TOTAL_W0] |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W1] << 16) |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W2] << 32) |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W3] << 48);

        if (lba48 > 0)
            total_sectors = lba48;
    }

    if (total_sectors > LBA28_MAX_SECTORS)
        needs_lba48 = 1;

    ata_copy_identify_string(serial, sizeof(serial),
                             &id[IDENTIFY_SERIAL_W0], 10);
    ata_copy_identify_string(model, sizeof(model),
                             &id[IDENTIFY_MODEL_W0], 20);
}

void ata_init(void)
{
    drive_ready = 0;
    total_sectors = 0;
    needs_lba48 = 0;
    model[0] = '\0';
    serial[0] = '\0';

    /* Select master drive, LBA mode, no bits of an LBA yet. */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_io_delay();

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);

    if (status == 0) {
        console_warn("ATA: no drive present (disk I/O disabled)");
        return;
    }

    if (ata_wait_not_busy() != 0) {
        console_warn("ATA: drive not responding (disk I/O disabled)");
        return;
    }

    /* A non-ATA (e.g. ATAPI) device reports its signature in the
     * LBA mid/high ports instead of raising DRQ for IDENTIFY. */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) {
        console_warn("ATA: non-ATA device present (disk I/O disabled)");
        return;
    }

    if (ata_wait_drq() != 0) {
        console_warn("ATA: IDENTIFY failed (disk I/O disabled)");
        return;
    }

    static uint16_t identify[IDENTIFY_WORDS];

    /* The DRQ must be drained either way; keep it for parsing. */
    for (int i = 0; i < IDENTIFY_WORDS; i++)
        identify[i] = inw(ATA_DATA);

    ata_parse_identify(identify);

    if (total_sectors == 0) {
        console_warn("ATA: drive reports zero capacity (disk I/O disabled)");
        return;
    }

    drive_ready = 1;
    console_info("ATA: primary master drive: OK");
    console_info("  model: ");
    terminal_write(model[0] ? model : "(unreported)");
    terminal_putchar('\n');
    console_info("  serial: ");
    terminal_write(serial[0] ? serial : "(unreported)");
    terminal_putchar('\n');
    console_info("  capacity: ");
    terminal_write_u32((uint32_t)(ata_total_bytes() / 1024U / 1024U));
    terminal_write(" MiB, ");
    terminal_write_u32((uint32_t)total_sectors);
    terminal_write(" sectors\n");

    if (needs_lba48) {
        /*
         * Not fatal for correctness as long as nothing addresses
         * past 128 GiB, but the disk is larger than this driver can
         * reach, so say so rather than pretending it is all there.
         */
        console_warn("ATA: drive needs LBA48; only the first 128 GiB are addressable");
    }
}

int ata_is_ready(void)
{
    return drive_ready;
}

uint64_t ata_total_sectors(void)
{
    return total_sectors;
}

uint64_t ata_total_bytes(void)
{
    return total_sectors * 512ULL;
}

const char *ata_model(void)
{
    return model;
}

const char *ata_serial(void)
{
    return serial;
}

int ata_requires_lba48(void)
{
    return needs_lba48;
}

int ata_check_range(uint32_t lba, uint32_t count)
{
    if (!drive_ready || count == 0)
        return -1;

    /* Compare in 64 bits so a lba near the top of the disk plus a
     * count can't wrap back into range. */
    if ((uint64_t)lba + (uint64_t)count > total_sectors)
        return -1;

    if (needs_lba48 && (uint64_t)lba + (uint64_t)count > LBA28_MAX_SECTORS)
        return -1;

    return 0;
}

static int ata_select_lba(uint32_t lba, uint8_t count)
{
    if (ata_wait_not_busy() != 0)
        return -1;

    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_delay();

    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));

    return 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer)
{
    if (buffer == 0)
        return -1;

    if (ata_check_range(lba, count) != 0)
        return -1;

    if (ata_select_lba(lba, count) != 0)
        return -1;

    outb(ATA_COMMAND, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {

        if (ata_wait_drq() != 0)
            return -1;

        for (int word = 0; word < 256; word++)
            dst[word] = inw(ATA_DATA);

        dst += 256;
    }

    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer)
{
    if (buffer == 0)
        return -1;

    if (ata_check_range(lba, count) != 0)
        return -1;

    if (ata_select_lba(lba, count) != 0)
        return -1;

    outb(ATA_COMMAND, ATA_CMD_WRITE);

    const uint16_t *src = (const uint16_t *)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {

        if (ata_wait_drq() != 0)
            return -1;

        for (int word = 0; word < 256; word++)
            outw(ATA_DATA, src[word]);

        src += 256;
    }

    /* Wait for the device to finish accepting the transfer before
     * flushing its write cache. */
    if (ata_wait_not_busy() != 0)
        return -1;

    outb(ATA_COMMAND, ATA_CMD_FLUSH);

    if (ata_wait_not_busy() != 0)
        return -1;

    return 0;
}

/*
 * Self-test.
 *
 * Deliberately read-only. A write probe would need a scratch sector
 * that the filesystem is guaranteed not to own, and there is no such
 * sector in the current layout -- picking one would risk clobbering
 * real file data. The read/write round-trip is covered by
 * fs_self_test() instead, which does own its scratch space.
 *
 * Returns non-zero on success, 0 on failure.
 */
int ata_run_self_test(void)
{
    if (!drive_ready) {
        console_error("ATA self-test: no drive present");
        return 0;
    }

    if (total_sectors == 0) {
        console_error("ATA self-test: capacity is zero");
        return 0;
    }

    if (ata_total_bytes() != total_sectors * 512ULL) {
        console_error("ATA self-test: byte/sector mismatch");
        return 0;
    }

    /* A range entirely inside the drive must be accepted. */
    if (ata_check_range(0, 1) != 0) {
        console_error("ATA self-test: LBA 0 wrongly rejected");
        return 0;
    }

    /* The very last sector must be reachable. */
    if (ata_check_range((uint32_t)(total_sectors - 1), 1) != 0) {
        console_error("ATA self-test: final sector wrongly rejected");
        return 0;
    }

    /* One sector past the end must be refused. */
    if (ata_check_range((uint32_t)total_sectors, 1) == 0) {
        console_error("ATA self-test: out-of-range LBA wrongly accepted");
        return 0;
    }

    /* A range that starts inside but runs past the end must be
     * refused, and must not wrap back into range. */
    if (ata_check_range((uint32_t)total_sectors, 2) == 0) {
        console_error("ATA self-test: wrapping range wrongly accepted");
        return 0;
    }

    if (total_sectors > 1 &&
        ata_check_range((uint32_t)(total_sectors - 1), 2) == 0) {
        console_error("ATA self-test: straddling range wrongly accepted");
        return 0;
    }

    /* A real read must succeed. */
    static uint8_t sector[512];

    if (ata_read_sectors(0, 1, sector) != 0) {
        console_error("ATA self-test: read of LBA 0 failed");
        return 0;
    }

    if (ata_read_sectors((uint32_t)total_sectors, 1, sector) == 0) {
        console_error("ATA self-test: out-of-range read wrongly succeeded");
        return 0;
    }

    console_info("ATA self-test: ");
    terminal_write_u32((uint32_t)(ata_total_bytes() / 1024U / 1024U));
    terminal_write(" MiB, model '");
    terminal_write(model[0] ? model : "(unreported)");
    terminal_write("'\n");

    return 1;
}
