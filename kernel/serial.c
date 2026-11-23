#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (u8)c);
}

void serial_write(const char *s)
{
    while (*s) serial_putc(*s++);
}

void serial_hex(u32 value)
{
    static const char digits[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(digits[(value >> i) & 0xF]);
}

void serial_hex64(u64 value)
{
    static const char digits[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(digits[(value >> i) & 0xF]);
}

void serial_dec(u32 value)
{
    char buf[12];
    int i = 0;
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (i > 0) serial_putc(buf[--i]);
}
