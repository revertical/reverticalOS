#ifndef RVOS_SERIAL_H
#define RVOS_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_hex(u32 value);
void serial_hex64(u64 value);
void serial_dec(u32 value);

#endif
