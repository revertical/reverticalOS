#include "rtc.h"
#include "io.h"

#define CMOS_PORT 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg)
{
    outb(CMOS_PORT, reg);
    return inb(CMOS_DATA);
}

static int bcd_to_bin(u8 v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

static int update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

void rtc_read(RtcTime *out)
{
    while (update_in_progress()) { }

    u8 sec = cmos_read(0x00);
    u8 min = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day = cmos_read(0x07);
    u8 month = cmos_read(0x08);
    u8 year = cmos_read(0x09);
    u8 status_b = cmos_read(0x0B);

    int binary = status_b & 0x04;
    int is_pm = hour & 0x80;
    int h = hour & 0x7F;

    if (!binary) {
        out->second = bcd_to_bin(sec);
        out->minute = bcd_to_bin(min);
        out->hour = bcd_to_bin((u8)h);
        out->day = bcd_to_bin(day);
        out->month = bcd_to_bin(month);
        out->year = bcd_to_bin(year);
    } else {
        out->second = sec;
        out->minute = min;
        out->hour = h;
        out->day = day;
        out->month = month;
        out->year = year;
    }

    if (!(status_b & 0x02)) {
        /* 12-hour clock */
        out->hour %= 12;
        if (is_pm) out->hour += 12;
    }

    out->year += 2000;
}
