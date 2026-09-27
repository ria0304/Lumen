#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/*
 * ATA driver supporting:
 * - Primary (0x1F0) and Secondary (0x170) IDE channels
 * - Master drives on each channel
 * - PIO and DMA (Bus Master IDE) transfers
 * - LBA28 addressing (128 GiB limit per drive)
 */

void ata_init(void);

/* Primary drive (0x1F0 master) */
int ata_is_ready(void);
uint64_t ata_total_sectors(void);
uint64_t ata_total_bytes(void);
const char *ata_model(void);
const char *ata_serial(void);
int ata_requires_lba48(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
int ata_check_range(uint32_t lba, uint32_t count);

/* Secondary drive (0x170 master) */
int ata2_is_ready(void);
uint64_t ata2_total_sectors(void);
uint64_t ata2_total_bytes(void);
const char *ata2_model(void);
const char *ata2_serial(void);
int ata2_requires_lba48(void);
int ata2_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata2_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
int ata2_check_range(uint32_t lba, uint32_t count);

/* DMA support */
int ata_dma_supported(void);

/* Self-test */
int ata_run_self_test(void);

#endif
