#ifndef RVOS_TIMER_H
#define RVOS_TIMER_H

#include "types.h"

void timer_init(void);
u64  timer_ms(void);
void timer_delay_ms(u32 ms);
u64  rdtsc(void);

#endif
