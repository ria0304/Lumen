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
 * Real capacity of the drive, in 512-byte sectors, read from the
 * IDENTIFY payload at init. Returns 0 if no drive.
 *
 * This is the number callers should bounds-check against. The disk
 * image is no longer assumed to be 4 MiB.
 */
uint64_t ata_total_sectors(void);

/* Same capacity, rounded down to whole mebibytes. */
uint64_t ata_total_bytes(void);

/*
 * Device identity strings from IDENTIFY, for `diskinfo`. Either may
 * be empty for a drive that reports blank fields.
 */
const char *ata_model(void);
const char *ata_serial(void);

/*
 * True if the drive reports more than 2^28-1 sectors, i.e. it needs
 * LBA48 to be addressed. This driver is LBA28-only, so such a drive
 * is detected and the excess reported at boot rather than silently
 * mis-addressed. 128 GiB is the ceiling.
 */
int ata_requires_lba48(void);

/*
 * Read/write 'count' consecutive 512-byte sectors starting at
 * LBA 'lba' into/from 'buffer'. 'buffer' must be at least
 * count * 512 bytes. Returns 0 on success, -1 on failure
 * (no drive, out-of-range LBA, or the controller reported an
 * error).
 */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

/*
 * Validate 'lba + count' against the drive's real capacity. Callers
 * (notably the filesystem) should run this before every access so a
 * corrupt or stale on-disk pointer cannot command a read outside
 * the disk. Returns 0 if the range is entirely within the drive.
 */
int ata_check_range(uint32_t lba, uint32_t count);

/*
 * Read-only self-test of capacity reporting and range checking.
 *
 * Follows the codebase convention: returns non-zero on success,
 * 0 on failure. Skipped (not failed) at boot when no drive is
 * present.
 */
int ata_run_self_test(void);

#endif
