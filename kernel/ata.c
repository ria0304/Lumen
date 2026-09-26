#include <stdint.h>
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

void ata_init(void)
{
    drive_ready = 0;

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

    /* Drain the 256-word IDENTIFY payload; nothing in it is used
     * yet, but the DRQ must be cleared before the next command. */
    for (int i = 0; i < 256; i++)
        (void)inw(ATA_DATA);

    drive_ready = 1;
    console_info("ATA: primary master drive: OK");
}

int ata_is_ready(void)
{
    return drive_ready;
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
    if (!drive_ready || count == 0 || buffer == 0)
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
    if (!drive_ready || count == 0 || buffer == 0)
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
