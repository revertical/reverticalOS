#ifndef RVOS_RTC_H
#define RVOS_RTC_H

#include "types.h"

typedef struct {
    int second;
    int minute;
    int hour;      /* 0..23 */
    int day;
    int month;
    int year;
} RtcTime;

void rtc_read(RtcTime *out);

#endif
