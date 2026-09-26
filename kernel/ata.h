#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/*
 * Minimal ATA PIO (LBA28) driver for the primary master hard
 * disk. This is a SEPARATE disk from the floppy image the boot
 * sector and kernel are loaded from -- QEMU is given a second
 * disk with '-hda build/disk.img', and this driver only ever
 * touches that one. It is polling PIO only: no IRQ14 handler,
 * no DMA, no LBA48, no slave/secondary-bus support, single
 * sector granularity per command.
 */

void ata_init(void);

/*
 * Returns 1 if a working ATA drive was found at init, 0
 * otherwise. Every read/write below is a no-op returning -1
 * when this is 0, so callers don't need to check it separately.
 */
int ata_is_ready(void);

/*
 * Read/write 'count' consecutive 512-byte sectors starting at
 * LBA 'lba' into/from 'buffer'. 'buffer' must be at least
 * count * 512 bytes. Returns 0 on success, -1 on failure
 * (no drive, or the controller reported an error).
 */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif
