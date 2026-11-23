#include "timer.h"
#include "io.h"
#include "serial.h"

static u64 tsc_per_ms;
static u64 tsc_at_boot;

u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void calibrate(void)
{
    /* PIT channel 2, mode 0, 50 ms one-shot.  OUT2 is low while the counter
       runs and goes high on terminal count, so we can bracket it with RDTSC. */
    u32 count = 1193182u / 20u;

    outb(0x61, (u8)((inb(0x61) & ~0x02u) | 0x01u));
    outb(0x43, 0xB0);
    outb(0x42, (u8)(count & 0xFF));
    outb(0x42, (u8)(count >> 8));

    /* Wait for OUT2 to drop: that means the counter has been armed. */
    for (u32 guard = 0; guard < 200000u; guard++) {
        if ((inb(0x61) & 0x20) == 0)
            break;
    }

    u64 t0 = rdtsc();
    for (u32 guard = 0; guard < 50000000u; guard++) {
        if (inb(0x61) & 0x20)
            break;
    }
    u64 t1 = rdtsc();

    u64 delta = t1 - t0;
    if (delta > 100000u)
        tsc_per_ms = delta / 50;
}

void timer_init(void)
{
    calibrate();
    if (tsc_per_ms < 1000)
        tsc_per_ms = 1000000;          /* fall back to a 1 GHz-ish clock */
    tsc_at_boot = rdtsc();

    serial_write("timer: ");
    serial_dec((u32)(tsc_per_ms / 1000));
    serial_write(" kHz TSC\r\n");
}

u64 timer_ms(void)
{
    return (rdtsc() - tsc_at_boot) / tsc_per_ms;
}

void timer_delay_ms(u32 ms)
{
    u64 end = timer_ms() + ms;
    while (timer_ms() < end) {
        __asm__ volatile ("pause");
    }
}
