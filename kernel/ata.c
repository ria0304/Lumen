#include <stdint.h>
#include <stddef.h>

/*
 * Set to 1 to enable the (currently misbehaving) DMA transfer path.
 * See the ata_init() comment for why this defaults to 0.
 */
#ifndef LUMEN_ATA_USE_DMA
#define LUMEN_ATA_USE_DMA 0
#endif

#include "ata.h"
#include "console.h"
#include "frame.h"
#include "paging.h"

static void print_hex8(uint8_t v){
    const char h[]="0123456789ABCDEF";
    terminal_putchar(h[v>>4]);
    terminal_putchar(h[v&0xF]);
}

static void print_hex32(uint32_t value);

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

/* Secondary ATA bus, I/O port block. */
#define ATA2_IO_BASE    0x170
#define ATA2_DATA       (ATA2_IO_BASE + 0)
#define ATA2_ERROR      (ATA2_IO_BASE + 1)
#define ATA2_SECCOUNT   (ATA2_IO_BASE + 2)
#define ATA2_LBA_LOW    (ATA2_IO_BASE + 3)
#define ATA2_LBA_MID    (ATA2_IO_BASE + 4)
#define ATA2_LBA_HIGH   (ATA2_IO_BASE + 5)
#define ATA2_DRIVE_HEAD (ATA2_IO_BASE + 6)
#define ATA2_STATUS     (ATA2_IO_BASE + 7)
#define ATA2_COMMAND    (ATA2_IO_BASE + 7)
#define ATA2_CONTROL    0x376

/*
 * Bus master IDE (PIIX) registers.
 *
 * These are I/O ports on the IDE controller, not memory: 0xC0 for the
 * primary channel and 0xD0 for the secondary. They used to be placed
 * at 0xD000, which is not a port at all.
 *
 * Every consequence of that was silent and terrible. Writing START to
 * an unmapped port did nothing, so the bus master engine never ran,
 * while reading the status register returned 0xFF -- and 0xFF has the
 * interrupt bit set, so the completion loop saw "finished" instantly
 * and reported success. The destination buffer was never written.
 *
 * Worse, the drive had already been issued READ DMA and was left
 * waiting for a transfer that would never come, so every subsequent
 * read -- including plain programmed I/O -- returned nothing either.
 *
 * Offsets, per the PIIX register map: status is readable at +0 and +2,
 * the command shares +2, and the PRDT address is at +4.
 */
#define ATA_BM_BASE     0xC0
#define ATA_BM_STATUS   (ATA_BM_BASE + 2)
#define ATA_BM_COMMAND  (ATA_BM_BASE + 2)
#define ATA_BM_PRDT     (ATA_BM_BASE + 4)

#define ATA_CMD_READ         0x20
#define ATA_CMD_WRITE        0x30
#define ATA_CMD_READ_DMA     0xC8
#define ATA_CMD_WRITE_DMA    0xCA
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_FLUSH        0xE7
#define ATA_CMD_FLUSH_EXT    0xEA

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_SRV  0x10
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_BSY  0x80

#define ATA_BM_CMD_START    0x01
#define ATA_BM_CMD_WRITE    0x08
#define ATA_BM_STATUS_INTR  0x04
#define ATA_BM_STATUS_ERR   0x02
#define ATA_BM_STATUS_ACT   0x01

#define PRDT_FLAG_EOT  0x80000000

static int drive_ready = 0;
static int drive_ready2 = 0;
static int dma_supported = 0;
static uint32_t dma_prdt_phys = 0;
static uint16_t *dma_prdt = 0;

/* Drive 0 (primary master) capacity and identity */
static uint64_t total_sectors = 0;
static char model[41];
static char serial[21];
static int needs_lba48 = 0;

/* Drive 1 (secondary master) capacity and identity */
static uint64_t total_sectors2 = 0;
static char model2[41];
static char serial2[21];
static int needs_lba48_2 = 0;

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

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
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

static void ata2_io_delay(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA2_CONTROL);
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

static int ata2_wait_not_busy(void)
{
    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        if ((inb(ATA2_STATUS) & ATA_STATUS_BSY) == 0)
            return 0;
    }
    return -1;
}

static int ata2_wait_drq(void)
{
    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        uint8_t status = inb(ATA2_STATUS);

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

static void ata_parse_identify2(const uint16_t *id)
{
    uint32_t lba28 = (uint32_t)id[IDENTIFY_LBA28_TOTAL_W0] |
                     ((uint32_t)id[IDENTIFY_LBA28_TOTAL_W1] << 16);

    total_sectors2 = lba28;
    needs_lba48_2 = 0;

    if (id[49] & IDENTIFY_LBA48_VALID) {
        uint64_t lba48 = (uint64_t)id[IDENTIFY_LBA48_TOTAL_W0] |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W1] << 16) |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W2] << 32) |
                         ((uint64_t)id[IDENTIFY_LBA48_TOTAL_W3] << 48);

        if (lba48 > 0)
            total_sectors2 = lba48;
    }

    if (total_sectors2 > LBA28_MAX_SECTORS)
        needs_lba48_2 = 1;

    ata_copy_identify_string(serial2, sizeof(serial2),
                             &id[IDENTIFY_SERIAL_W0], 10);
    ata_copy_identify_string(model2, sizeof(model2),
                             &id[IDENTIFY_MODEL_W0], 20);
}

static void print_hex32(uint32_t value)
{
    const char hex[] = "0123456789ABCDEF";
    terminal_write("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t digit = (value >> (i * 4)) & 0xF;
        terminal_putchar(hex[digit]);
    }
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)(slot & 0x1F) << 11)
                     | ((uint32_t)(func & 0x07) << 8)
                     | (off & 0xFC);

    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t value)
{
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)(slot & 0x1F) << 11)
                     | ((uint32_t)(func & 0x07) << 8)
                     | (off & 0xFC);

    outl(0xCF8, address);
    outl(0xCFC, value);
}

/*
 * Turn on Bus Mastering for the IDE controller.
 *
 * Without bit 2 of the PCI command register set, the controller
 * never takes ownership of the bus: no interrupt is ever raised and
 * no sector is moved. Locate the IDE function by its PCI class code
 * (0x0101) rather than hardcoding PIIX slot/function, which differ
 * between QEMU machine types.
 */
static void ide_enable_bus_master(void)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {

            uint32_t id = pci_cfg_read32(0, slot, func, 0x00);

            if (id == 0xFFFFFFFFu)
                continue;               /* no device here */

            /* dword at 0x08: revision [7:0], prog-if [15:8],
             * subclass [23:16], class [31:24] */
            uint32_t class_reg = pci_cfg_read32(0, slot, func, 0x08);
            uint32_t class_code = (class_reg >> 24) & 0xFF;
            uint32_t subclass = (class_reg >> 16) & 0xFF;

            if (class_code != 0x01 || subclass != 0x01)
                continue;

            uint32_t command = pci_cfg_read32(0, slot, func, 0x04);

            if (command & 0x04)
                return;                 /* already enabled */

            pci_cfg_write32(0, slot, func, 0x04, command | 0x04);

            console_info("DMA: bus master enabled on IDE");
            terminal_putchar('\n');
            return;
        }
    }

    console_warn("DMA: no IDE PCI function found for bus mastering");
}

/* DMA PRDT entry: 8 bytes (physical address + byte count + EOT flag) */
static int dma_init(void)
{
    /* Allocate a physically contiguous PRDT (Physical Region Descriptor Table)
     * We need one PRDT per transfer, max 16 entries = 128 bytes.
     * The PRDT must not cross a 64KB boundary. */
    dma_prdt_phys = frame_alloc();
    if (dma_prdt_phys == FRAME_INVALID || dma_prdt_phys >= PAGING_IDENTITY_LIMIT) {
        if (dma_prdt_phys != FRAME_INVALID)
            frame_free(dma_prdt_phys);
        console_warn("DMA: PRDT allocation failed");
        return -1;
    }

    dma_prdt = (uint16_t *)dma_prdt_phys;
    console_info("DMA: PRDT allocated at 0x");
    print_hex32(dma_prdt_phys);
    terminal_putchar('\n');

    ide_enable_bus_master();

    return 0;
}

static void dma_setup_prdt(void *buffer, uint32_t byte_count, int is_write)
{
    /* We use a single PRDT entry for simplicity (max 64KB per entry) */
    uint32_t phys = (uint32_t)buffer;
    uint32_t remaining = byte_count;
    uint32_t *prdt = (uint32_t *)dma_prdt;

    /* Each PRDT entry: 4 bytes addr, 4 bytes count (with EOT flag in bit 31) */
    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > 65536)
            chunk = 65536;

        /* Address (4 bytes) */
        *prdt++ = phys;

        /* Byte count (4 bytes) with EOT flag */
        uint32_t count_val = chunk;
        if (chunk == remaining)
            count_val |= 0x80000000;  /* EOT flag */
        *prdt++ = count_val;

        phys += chunk;
        remaining -= chunk;
    }
}

void ata_init(void)
{
    drive_ready = 0;
    drive_ready2 = 0;
    total_sectors = 0;
    total_sectors2 = 0;
    needs_lba48 = 0;
    needs_lba48_2 = 0;
    model[0] = '\0';
    serial[0] = '\0';
    model2[0] = '\0';
    serial2[0] = '\0';
    dma_supported = 0;
    dma_prdt_phys = 0;
    dma_prdt = 0;

    /* Initialize DMA if possible.
     *
     * Off by default. The DMA path completes without reporting an
     * error under QEMU but leaves the destination buffer holding the
     * wrong bytes, so every filesystem read came back wrong while the
     * call claimed success. Until that is root-caused, PIO is the
     * correct choice: it is slower but it is right. Build with
     * -DLUMEN_ATA_USE_DMA=1 to opt back in.
     */
#if LUMEN_ATA_USE_DMA
    if (dma_init() == 0) {
        dma_supported = 1;
        console_info("ATA: DMA support enabled");
    } else {
        console_warn("ATA: DMA not available, using PIO");
    }
#else
    console_info("ATA: using PIO (DMA disabled)");
#endif

    /* ---- Primary Master (0x1F0) ---- */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_io_delay();

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);

    if (status == 0) {
        console_warn("ATA: no primary drive present");
    } else if (ata_wait_not_busy() == 0) {
        if (inb(ATA_LBA_MID) == 0 && inb(ATA_LBA_HIGH) == 0) {
            if (ata_wait_drq() == 0) {
                static uint16_t identify[IDENTIFY_WORDS];
                for (int i = 0; i < IDENTIFY_WORDS; i++)
                    identify[i] = inw(ATA_DATA);
                ata_parse_identify(identify);

                if (total_sectors != 0) {
                    drive_ready = 1;
                    console_info("ATA: primary master drive: OK");
                    console_info("  model: ");
                    terminal_write(model[0] ? model : "(unreported)");
                    terminal_putchar('\n');
                    console_info("  serial: ");
                    terminal_write(serial[0] ? serial : "(unreported)");
                    terminal_putchar('\n');
                    console_info("  capacity: ");
                    terminal_write_u32((uint32_t)(total_sectors * 512ULL / 1024U / 1024U));
                    terminal_write(" MiB, ");
                    terminal_write_u32((uint32_t)total_sectors);
                    terminal_write(" sectors\n");
                    if (needs_lba48) {
                        console_warn("ATA: primary needs LBA48; only first 128 GiB addressable");
                    }
                }
            }
        }
    }

    /* ---- Secondary Master (0x170) ---- */
    outb(ATA2_DRIVE_HEAD, 0xE0);
    ata2_io_delay();

    outb(ATA2_SECCOUNT, 0);
    outb(ATA2_LBA_LOW, 0);
    outb(ATA2_LBA_MID, 0);
    outb(ATA2_LBA_HIGH, 0);
    outb(ATA2_COMMAND, ATA_CMD_IDENTIFY);

    status = inb(ATA2_STATUS);

    if (status == 0) {
        console_warn("ATA: no secondary drive present");
    } else if (ata2_wait_not_busy() == 0) {
        if (inb(ATA2_LBA_MID) == 0 && inb(ATA2_LBA_HIGH) == 0) {
            if (ata2_wait_drq() == 0) {
                static uint16_t identify2[IDENTIFY_WORDS];
                for (int i = 0; i < IDENTIFY_WORDS; i++)
                    identify2[i] = inw(ATA2_DATA);
                ata_parse_identify2(identify2);

                if (total_sectors2 != 0) {
                    drive_ready2 = 1;
                    console_info("ATA: secondary master drive: OK");
                    console_info("  model: ");
                    terminal_write(model2[0] ? model2 : "(unreported)");
                    terminal_putchar('\n');
                    console_info("  serial: ");
                    terminal_write(serial2[0] ? serial2 : "(unreported)");
                    terminal_putchar('\n');
                    console_info("  capacity: ");
                    terminal_write_u32((uint32_t)(total_sectors2 * 512ULL / 1024U / 1024U));
                    terminal_write(" MiB, ");
                    terminal_write_u32((uint32_t)total_sectors2);
                    terminal_write(" sectors\n");
                    if (needs_lba48_2) {
                        console_warn("ATA: secondary needs LBA48; only first 128 GiB addressable");
                    }
                }
            }
        }
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

/* Secondary drive getters */
int ata2_is_ready(void)
{
    return drive_ready2;
}

uint64_t ata2_total_sectors(void)
{
    return total_sectors2;
}

uint64_t ata2_total_bytes(void)
{
    return total_sectors2 * 512ULL;
}

const char *ata2_model(void)
{
    return model2;
}

const char *ata2_serial(void)
{
    return serial2;
}

int ata2_requires_lba48(void)
{
    return needs_lba48_2;
}

int ata_dma_supported(void)
{
    return dma_supported;
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

static int ata2_select_lba(uint32_t lba, uint8_t count)
{
    if (ata2_wait_not_busy() != 0)
        return -1;

    outb(ATA2_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata2_io_delay();

    outb(ATA2_SECCOUNT, count);
    outb(ATA2_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA2_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA2_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));

    return 0;
}

static int ata_dma_read(uint32_t lba, uint8_t count, void *buffer)
{
    if (!dma_supported || dma_prdt == 0)
        return -1;

    uint32_t byte_count = count * 512;

    /* Setup PRDT */
    dma_setup_prdt(buffer, byte_count, 0);

    /* Program the PRDT address */
    outl(ATA_BM_PRDT, dma_prdt_phys);

    /* Clear status */
    outb(ATA_BM_STATUS, ATA_BM_STATUS_INTR | ATA_BM_STATUS_ERR);

    /* Select drive and LBA */
    if (ata_select_lba(lba, count) != 0)
        return -1;

    /* Issue DMA read command */
    outb(ATA_COMMAND, ATA_CMD_READ_DMA);

    /* Start the bus master after the drive has been told to fetch. */
    outb(ATA_BM_COMMAND, ATA_BM_CMD_START);

    /* Wait for completion. A spin timeout means the transfer never
     * happened -- report failure so the caller falls back to PIO.
     * Returning success here used to leave 'buffer' untouched while
     * claiming the read worked, which silently handed the filesystem
     * a sector full of garbage. */
    int completed = 0;

    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        uint8_t status = inb(ATA_BM_STATUS);

        /* Error first. A failed transfer sets both bits, and testing
         * the interrupt first reported success on every one of them. */
        if (status & ATA_BM_STATUS_ERR) {
            console_error("DMA: transfer error");
            break;
        }

        if (status & ATA_BM_STATUS_INTR) {
            completed = 1;
            break;
        }
    }

    /* Stop DMA */
    outb(ATA_BM_COMMAND, 0);

    if (!completed) {
        /* Clear the pending error/interrupt so the drive is left in a
         * state where the PIO fallback can drive it again. */
        outb(ATA_BM_STATUS, ATA_BM_STATUS_INTR | ATA_BM_STATUS_ERR);
        return -1;
    }

    /* Wait for drive to be ready */
    if (ata_wait_not_busy() != 0)
        return -1;

    return 0;
}

static int ata_dma_write(uint32_t lba, uint8_t count, const void *buffer)
{
    if (!dma_supported || dma_prdt == 0)
        return -1;

    uint32_t byte_count = count * 512;

    /* Setup PRDT */
    dma_setup_prdt((void *)buffer, byte_count, 1);

    /* Program the PRDT address */
    outl(ATA_BM_PRDT, dma_prdt_phys);

    /* Clear status */
    outb(ATA_BM_STATUS, ATA_BM_STATUS_INTR | ATA_BM_STATUS_ERR);

    /* Select drive and LBA */
    if (ata_select_lba(lba, count) != 0)
        return -1;

    /* Issue DMA write command */
    outb(ATA_COMMAND, ATA_CMD_WRITE_DMA);

    /* Start DMA transfer */
    outb(ATA_BM_COMMAND, ATA_BM_CMD_START | ATA_BM_CMD_WRITE);

    /* Wait for completion. As with reads, a spin timeout is a
     * failure -- reporting success would leave the caller believing
     * data reached the platter when nothing was written. */
    int completed = 0;

    for (uint32_t spins = 0; spins < 1000000U; spins++) {
        uint8_t status = inb(ATA_BM_STATUS);
        if (status & ATA_BM_STATUS_INTR) {
            completed = 1;
            break;
        }
        if (status & ATA_BM_STATUS_ERR) {
            console_error("DMA: transfer error");
            break;
        }
    }

    /* Stop DMA */
    outb(ATA_BM_COMMAND, 0);

    if (!completed) {
        outb(ATA_BM_STATUS, ATA_BM_STATUS_INTR | ATA_BM_STATUS_ERR);
        return -1;
    }

    /* Wait for drive to be ready */
    if (ata_wait_not_busy() != 0)
        return -1;

    /* Flush cache */
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy() != 0)
        return -1;

    return 0;
}

/* Programmed I/O read, bypassing DMA entirely. */
static int ata_pio_read(uint32_t lba, uint8_t count, void *buffer)
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

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer)
{
    if (buffer == 0)
        return -1;

    if (ata_check_range(lba, count) != 0)
        return -1;

    /* Try DMA first if supported */
    if (dma_supported && dma_prdt != 0) {
        if (ata_dma_read(lba, count, buffer) == 0)
            return 0;
        console_warn("DMA read failed, falling back to PIO");
    }

    return ata_pio_read(lba, count, buffer);
}

int ata2_read_sectors(uint32_t lba, uint8_t count, void *buffer)
{
    if (buffer == 0)
        return -1;

    if (ata2_check_range(lba, count) != 0)
        return -1;

    if (ata2_select_lba(lba, count) != 0)
        return -1;

    outb(ATA2_COMMAND, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata2_wait_drq() != 0)
            return -1;

        for (int word = 0; word < 256; word++)
            dst[word] = inw(ATA2_DATA);

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

    /* Try DMA first if supported */
    if (dma_supported && dma_prdt != 0) {
        if (ata_dma_write(lba, count, buffer) == 0)
            return 0;
        console_warn("DMA write failed, falling back to PIO");
    }

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

int ata2_write_sectors(uint32_t lba, uint8_t count, const void *buffer)
{
    if (buffer == 0)
        return -1;

    if (ata2_check_range(lba, count) != 0)
        return -1;

    if (ata2_select_lba(lba, count) != 0)
        return -1;

    outb(ATA2_COMMAND, ATA_CMD_WRITE);

    const uint16_t *src = (const uint16_t *)buffer;

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata2_wait_drq() != 0)
            return -1;

        for (int word = 0; word < 256; word++)
            outw(ATA2_DATA, src[word]);

        src += 256;
    }

    if (ata2_wait_not_busy() != 0)
        return -1;

    outb(ATA2_COMMAND, ATA_CMD_FLUSH);

    if (ata2_wait_not_busy() != 0)
        return -1;

    return 0;
}

int ata2_check_range(uint32_t lba, uint32_t count)
{
    if (!drive_ready2 || count == 0)
        return -1;

    if ((uint64_t)lba + (uint64_t)count > total_sectors2)
        return -1;

    if (needs_lba48_2 && (uint64_t)lba + (uint64_t)count > LBA28_MAX_SECTORS)
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
        console_error("ATA self-test: no primary drive present");
        return 0;
    }

    if (total_sectors == 0) {
        console_error("ATA self-test: primary capacity is zero");
        return 0;
    }

    if (ata_total_bytes() != total_sectors * 512ULL) {
        console_error("ATA self-test: primary byte/sector mismatch");
        return 0;
    }

    /* A range entirely inside the drive must be accepted. */
    if (ata_check_range(0, 1) != 0) {
        console_error("ATA self-test: primary LBA 0 wrongly rejected");
        return 0;
    }

    /* The very last sector must be reachable. */
    if (ata_check_range((uint32_t)(total_sectors - 1), 1) != 0) {
        console_error("ATA self-test: primary final sector wrongly rejected");
        return 0;
    }

    /* One sector past the end must be refused. */
    if (ata_check_range((uint32_t)total_sectors, 1) == 0) {
        console_error("ATA self-test: primary out-of-range LBA wrongly accepted");
        return 0;
    }

    /* A range that starts inside but runs past the end must be
     * refused, and must not wrap back into range. */
    if (ata_check_range((uint32_t)total_sectors, 2) == 0) {
        console_error("ATA self-test: primary wrapping range wrongly accepted");
        return 0;
    }

    if (total_sectors > 1 &&
        ata_check_range((uint32_t)(total_sectors - 1), 2) == 0) {
        console_error("ATA self-test: primary straddling range wrongly accepted");
        return 0;
    }

    /* A real read must succeed. */
    static uint8_t sector[512];

    if (ata_read_sectors(0, 1, sector) != 0) {
        console_error("ATA self-test: primary read of LBA 0 failed");
        return 0;
    }

    /*
     * Whatever path served that read must have delivered the same
     * bytes a plain programmed I/O read would.
     *
     * When DMA is enabled this compares the two directly, and again with
     * a buffer on the stack because that is what the filesystem uses
     * for its superblock. The DMA path used to report success while
     * leaving the buffer untouched, so every filesystem read returned
     * whatever happened to be in memory -- silently, with no error
     * anywhere. Checking here turns that into a visible failure.
     *
     * Status with LUMEN_ATA_USE_DMA=1: a statically placed buffer
     * matches PIO, but a stack buffer diverges partway through the
     * sector (observed at 0x0008FB98, first difference at byte 236).
     * The failure is left in place deliberately, so enabling DMA is a
     * red build rather than a silent one.
     */
    if (dma_supported && dma_prdt != 0) {
        static uint8_t via_pio[512];
        static uint8_t via_dma[512];

        /* One side must be a real PIO read: ata_read_sectors() would
         * have taken the DMA path too, making the comparison
         * vacuous. */
        if (ata_pio_read(0, 1, via_pio) != 0 ||
            ata_read_sectors(0, 1, via_dma) != 0) {

            console_error("ATA self-test: DMA comparison read failed");
            return 0;
        }

        for (uint32_t i = 0; i < 512; i++) {
            if (via_pio[i] != via_dma[i]) {
                console_error("ATA self-test: DMA read differs from PIO");
                terminal_write("  first difference at byte ");
                terminal_write_u32(i);
                terminal_write(", PIO=");
                print_hex8(via_pio[i]);
                terminal_write(" DMA=");
                print_hex8(via_dma[i]);
                terminal_write("\n");
                return 0;
            }
        }

        /*
         * Repeat with a buffer on the stack. The filesystem reads its
         * superblock into a stack local, so a path that only works for
         * statically placed buffers would pass the check above and
         * still leave the filesystem unable to mount.
         */
        {
            uint8_t stack_dma[512];
            uint8_t stack_pio[512];

            if (ata_read_sectors(0, 1, stack_dma) != 0 ||
                ata_pio_read(0, 1, stack_pio) != 0) {

                console_error("ATA self-test: stack comparison read failed");
                return 0;
            }

            for (uint32_t i = 0; i < 512; i++) {
                if (stack_dma[i] != stack_pio[i]) {
                    console_error(
                        "ATA self-test: DMA differs from PIO for a "
                        "stack buffer"
                    );
                    terminal_write("  address ");
                    print_hex32((uint32_t)(uintptr_t)stack_dma);
                    terminal_write(" first differs at byte ");
                    terminal_write_u32(i);
                    terminal_putchar('\n');
                    return 0;
                }
            }
        }
    }

    if (ata_read_sectors((uint32_t)total_sectors, 1, sector) == 0) {
        console_error("ATA self-test: primary out-of-range read wrongly succeeded");
        return 0;
    }

    console_info("ATA self-test: primary ");
    terminal_write_u32((uint32_t)(ata_total_bytes() / 1024U / 1024U));
    terminal_write(" MiB, model '");
    terminal_write(model[0] ? model : "(unreported)");
    terminal_write("'\n");

    /* Test secondary drive if present */
    if (drive_ready2) {
        if (ata2_read_sectors(0, 1, sector) != 0) {
            console_error("ATA self-test: secondary read of LBA 0 failed");
            return 0;
        }
        console_info("ATA self-test: secondary OK");
    }

    return 1;
}
