#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* Broken-down local time as reported by the CMOS RTC. */
typedef struct {
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t month;   /* 1-12 */
    uint8_t day;     /* 1-31 */
    uint8_t hour;    /* 0-23 */
    uint8_t minute;  /* 0-59 */
    uint8_t second;  /* 0-59 */
    uint8_t weekday; /* 0 = Sunday .. 6 = Saturday */
} rtc_time_t;

/*
 * Read the CMOS real-time clock. Returns 0 on success, -1 if the
 * register pair would not settle.
 *
 * The RTC keeps time in its own registers; this converts the
 * packed-BCD values to plain binary and reconstructs the full year
 * from the century register, so callers never deal with BCD.
 */
int rtc_read(rtc_time_t *out);

/*
 * True if the hardware clock is running in 24-hour mode. Read once
 * from the status register B; needed to interpret the hour
 * register.
 */
int rtc_is_24_hour(void);

/*
 * True if the century register holds a real century rather than
 * echoing the year. Some very old boards always report 0x00 or
 * 0xFF here, in which case the year is derived from a fixed pivot
 * instead. Probed once at init.
 */
int rtc_has_century(void);

/* Format as "YYYY-MM-DD HH:MM:SS" into a caller buffer of >= 20
 * bytes. */
void rtc_format(const rtc_time_t *t, char *buffer, uint32_t size);

/*
 * Self-test. Returns non-zero when the clock reads back as a
 * plausible time (or is absent, in which case the fallback date is in
 * use and this is reported as a pass), 0 if the values are
 * inconsistent.
 */
int rtc_run_self_test(void);

#endif
