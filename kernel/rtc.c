#include <stdint.h>
#include "rtc.h"
#include "console.h"

/*
 * CMOS/RTC port block. Index ports are selected by writing a register
 * number to CMOS_INDEX, then the value is read from CMOS_DATA.
 */
#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECONDS      0x00
#define RTC_MINUTES      0x02
#define RTC_HOURS        0x04
#define RTC_WEEKDAY      0x06
#define RTC_DAY          0x07
#define RTC_MONTH        0x08
#define RTC_YEAR         0x09
#define RTC_CENTURY      0x32
#define RTC_STATUS_A     0x0A
#define RTC_STATUS_B     0x0B

/* Status register B. */
#define RTC_B_24HOUR     0x02
#define RTC_B_BINARY     0x04 /* 1 = binary, 0 = BCD */
#define RTC_B_UTC        0x40

/* Status register A. */
#define RTC_A_UIP        0x80 /* update in progress */
#define RTC_A_DM         0x40 /* binary mode, mirrors STATUS_B */
#define RTC_A_24HOUR     0x02

/*
 * The RTC battery is dead on some machines, and some VMs have no RTC
 * at all. Rather than hard-fail, probe once and fall back to a
 * plausible fixed date so timestamps stay monotonic and the
 * filesystem still works.
 */
#define RTC_FALLBACK_YEAR  2024
#define RTC_FALLBACK_MONTH 1
#define RTC_FALLBACK_DAY   1

/* Cached flags, probed on first read. */
static int probe_done = 0;
static int use_24_hour = 0;
static int have_century = 0;

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

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

/*
 * Convert a CMOS byte to plain binary. The RTC may be in BCD or
 * binary mode depending on STATUS_B, and when the CMOS battery is
 * dead the registers can read back as plain binary regardless of
 * what the mode bit claims, which is why this sniffs the high nibble
 * rather than trusting the flag.
 */
static uint8_t decode_bcd(uint8_t value, int force_binary)
{
    if (force_binary)
        return value;

    uint8_t high = (uint8_t)((value >> 4) & 0x0F);
    uint8_t low = value & 0x0F;

    /* A valid BCD nibble pair is 0x00-0x99. Anything else means the
     * register is already binary (or garbage), so pass it through. */
    if (high > 9 || low > 9)
        return value;

    return (uint8_t)(high * 10 + low);
}

static void rtc_probe(void)
{
    if (probe_done)
        return;

    uint8_t status_b = cmos_read(RTC_STATUS_B);

    use_24_hour = (status_b & RTC_B_24HOUR) ? 1 : 0;

    /*
     * Some boards leave STATUS_B bit 1 (binary mode) clear while the
     * registers themselves hold plain binary anyway. rtc_read
     * detects that per-read by checking which interpretation yields a
     * legal time, so there is nothing to decide here.
     */

    /*
     * On some boards the century register always reads 0x00 or 0xFF.
     * 0x00 is ambiguous (year 2000 vs 1900), 0xFF is not a year at
     * all. Treat 0xFF as "no century" and 0x00 as usable, since a
     * zero century is at least a legal value.
     */
    uint8_t century = cmos_read(RTC_CENTURY);
    have_century = (century != 0xFF) ? 1 : 0;

    probe_done = 1;
}

int rtc_is_24_hour(void)
{
    rtc_probe();
    return use_24_hour;
}

int rtc_has_century(void)
{
    rtc_probe();
    return have_century;
}

/*
 * Wait for an update-in-progress to finish before reading. Reading
 * mid-update can return seconds 59 followed by 00 in the wrong
 * order, or worse, a torn date around midnight.
 */
static int rtc_wait_update_done(void)
{
    for (int spins = 0; spins < 100000; spins++) {
        if ((cmos_read(RTC_STATUS_A) & RTC_A_UIP) == 0)
            return 0;
    }

    return -1;
}

int rtc_read(rtc_time_t *out)
{
    if (out == 0)
        return -1;

    rtc_probe();

    if (rtc_wait_update_done() != 0)
        return -1;

    uint8_t second = cmos_read(RTC_SECONDS);
    uint8_t minute = cmos_read(RTC_MINUTES);
    uint8_t hour = cmos_read(RTC_HOURS);
    uint8_t weekday = cmos_read(RTC_WEEKDAY);
    uint8_t day = cmos_read(RTC_DAY);
    uint8_t month = cmos_read(RTC_MONTH);
    uint8_t year = cmos_read(RTC_YEAR);
    uint8_t century = cmos_read(RTC_CENTURY);

    /*
     * STATUS_B bit 1 (0x02) and STATUS_A bit 2 (0x04) both declare
     * binary mode, and either may be trustworthy. Read both and
     * accept binary if either says so.
     */
    uint8_t status_a = cmos_read(RTC_STATUS_A);
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int binary = ((status_b & RTC_B_BINARY) || (status_a & RTC_A_DM)) ? 1 : 0;

    /*
     * Re-read if the second changed between the first and last read;
     * an update landed in the middle and the values are torn.
     */
    if (cmos_read(RTC_SECONDS) != second) {
        second = cmos_read(RTC_SECONDS);
        minute = cmos_read(RTC_MINUTES);
        hour = cmos_read(RTC_HOURS);
        day = cmos_read(RTC_DAY);
        month = cmos_read(RTC_MONTH);
        year = cmos_read(RTC_YEAR);
        century = cmos_read(RTC_CENTURY);
    }

    uint8_t seconds = decode_bcd(second, binary);

    /*
     * Some boards leave the binary-mode bit clear while the registers
     * hold plain binary. Seconds in 0-59 is true under both
     * interpretations, but only one of the two decodes lands in
     * range, so try the other when this one does not.
     */
    if (seconds > 59) {
        binary = !binary;
        seconds = decode_bcd(second, binary);
    }

    out->second = seconds;
    out->minute = decode_bcd(minute, binary);
    out->day = decode_bcd(day, binary);
    out->month = decode_bcd(month, binary);
    out->year = decode_bcd(year, binary);
    out->weekday = decode_bcd(weekday, binary);

    if (!use_24_hour) {
        /*
         * 12-hour mode: bit 7 of the hour register is PM and the low
         * bits are 1-12, not 0-23. Convert to 24-hour.
         */
        int pm = (hour & 0x80) ? 1 : 0;
        uint8_t h12 = (uint8_t)(hour & 0x7F);
        h12 = decode_bcd(h12, binary);

        if (h12 == 12)
            h12 = 0;

        out->hour = (uint8_t)(h12 + (pm ? 12 : 0));
    } else {
        out->hour = decode_bcd(hour, binary);
    }

    /* Reconstruct the full year. */
    if (have_century) {
        uint16_t cent = decode_bcd(century, binary);
        out->year = (uint16_t)(cent * 100 + out->year);
    } else {
        /* No usable century register: assume the modern pivot and
         * map 00-79 to this century, 80-99 to the previous one, which
         * is the conventional interpretation. */
        if (out->year >= 80)
            out->year = (uint16_t)(1900 + out->year);
        else
            out->year = (uint16_t)(2000 + out->year);
    }

    /* Sanity-check the result. A dead battery or an absent RTC can
     * read back as zeros or as 0xFF, and writing those into file
     * timestamps would poison the filesystem. */
    if (out->month < 1 || out->month > 12 ||
        out->day < 1 || out->day > 31 ||
        out->hour > 23 || out->minute > 59 || out->second > 59 ||
        out->year < 1980 || out->year > 2107) {

        out->year = RTC_FALLBACK_YEAR;
        out->month = RTC_FALLBACK_MONTH;
        out->day = RTC_FALLBACK_DAY;
        out->hour = 0;
        out->minute = 0;
        out->second = 0;
        out->weekday = 1;

        return -1;
    }

    return 0;
}

void rtc_format(const rtc_time_t *t, char *buffer, uint32_t size)
{
    if (t == 0 || buffer == 0 || size == 0)
        return;

    char *p = buffer;
    char *end = buffer + size - 1;

    const uint32_t values[6] = {
        t->year, t->month, t->day, t->hour, t->minute, t->second
    };
    const uint32_t widths[6] = { 4, 2, 2, 2, 2, 2 };
    const char seps[5] = { '-', '-', ' ', ':', ':' };

    for (int i = 0; i < 6; i++) {
        uint32_t v = values[i];

        /* Place the digits right-aligned in their field by starting
         * from the largest power of ten that fits. */
        uint32_t divisor = 1;

        for (uint32_t d = 1; d < widths[i]; d++)
            divisor *= 10;

        for (uint32_t d = 0; d < widths[i]; d++) {
            if (p >= end)
                break;

            *p++ = (char)('0' + (v / divisor) % 10);

            if (divisor > 1)
                divisor /= 10;
        }

        if (i < 5 && p < end)
            *p++ = seps[i];
    }

    *p = '\0';
}

/*
 * Self-test: the clock must produce a plausible, in-range time, and
 * rtc_format() must lay it out as expected. Returns non-zero on
 * success, 0 on failure.
 */
int rtc_run_self_test(void)
{
    rtc_time_t t;

    if (rtc_read(&t) != 0) {

        /* rtc_read() substitutes a fallback date and reports the
         * failure, so an absent or dead RTC is a skip rather than a
         * kernel fault. */
        console_info("RTC self-test: clock unavailable, using fallback date\n");
        return 0;
    }

    if (t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 ||
        t.hour > 23 || t.minute > 59 || t.second > 59) {
        console_error("RTC self-test: time out of range");
        return 0;
    }

    if (t.year < 1980 || t.year > 2107) {
        console_error("RTC self-test: year out of range");
        return 0;
    }

    /* 12-hour mode must have been normalised away by now. */
    if (t.hour > 23) {
        console_error("RTC self-test: hour not normalised to 24h");
        return 0;
    }

    char buffer[32];
    rtc_format(&t, buffer, sizeof(buffer));

    console_info("RTC self-test: ");
    terminal_write(buffer);
    terminal_write(rtc_is_24_hour() ? " (24h)" : " (12h normalised)");
    terminal_write(rtc_has_century() ? ", century ok" : ", no century reg");
    terminal_putchar('\n');

    /* A second read should still be sane, which is a cheap check that
     * the update-in-progress wait is actually working. */
    rtc_time_t t2;

    if (rtc_read(&t2) != 0) {
        console_error("RTC self-test: second read failed");
        return 0;
    }

    if (t2.hour > 23 || t2.minute > 59 || t2.second > 59) {
        console_error("RTC self-test: second read out of range");
        return 0;
    }

    return 1;
}
