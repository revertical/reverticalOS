/* Kernel panic screen.
 *
 * Deliberately self-contained: a panic can happen before the font cache, the
 * desktop or even the framebuffer exists.  The screen therefore uses its own
 * 5x7 bitmap font and falls back to VGA text plus COM1 when there is no
 * graphics console yet.
 */
#include "panic.h"
#include "gfx.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* 5x7 glyphs for ASCII 32..95, five columns per glyph. */
static const u8 font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}
};

static volatile int in_panic;

static const char *const exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualisation", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "hypervisor", "VMM communication", "security", "reserved"
};

static char *append(char *p, char *end, const char *s)
{
    while (*s && p < end - 1)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *append_hex(char *p, char *end, u32 value, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        if (p < end - 1)
            *p++ = hex[(value >> (i * 4)) & 0xF];
    }
    *p = 0;
    return p;
}

static char *append_dec(char *p, char *end, u32 value)
{
    char tmp[12];
    int n = 0;
    if (value == 0)
        tmp[n++] = '0';
    while (value) {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (n && p < end - 1)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static void vga_line(int row, const char *text, u8 attr)
{
    volatile u16 *vga = (volatile u16 *)0xB8000;
    int col = 0;
    while (text[col] && col < 80) {
        char c = text[col];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        vga[row * 80 + col] = (u16)(attr << 8) | (u8)c;
        col++;
    }
    while (col < 80) {
        vga[row * 80 + col] = (u16)(attr << 8) | ' ';
        col++;
    }
}

static void vga_screen(const char *reason, u32 vector, u32 error,
                       u32 eip, u32 cs, u32 eflags)
{
    volatile u16 *vga = (volatile u16 *)0xB8000;
    for (int i = 0; i < 80 * 25; i++)
        vga[i] = (u16)(0x1F << 8) | ' ';

    char line[96];
    char *p = line;
    char *end = line + sizeof(line);
    vga_line(1, "REVERTICALOS HAS STOPPED", 0x4F);
    vga_line(3, reason, 0x1F);

    if (vector != 0xFFFFFFFFu) {
        p = append(p, end, "EXCEPTION #");
        p = append_dec(p, end, vector);
        p = append(p, end, "  ");
        p = append(p, end, exception_names[vector & 31]);
        vga_line(6, line, 0x1F);
        p = line;
        p = append(p, end, "ERROR 0x");
        p = append_hex(p, end, error, 8);
        vga_line(7, line, 0x1F);
        p = line;
        p = append(p, end, "EIP 0x");
        p = append_hex(p, end, eip, 8);
        p = append(p, end, "  CS 0x");
        p = append_hex(p, end, cs, 4);
        p = append(p, end, "  FLAGS 0x");
        p = append_hex(p, end, eflags, 8);
        vga_line(8, line, 0x1F);
    }

    vga_line(23, "THE SYSTEM IS HALTED. PRESS CTRL+ALT+DEL TO RESTART.", 0x4F);
}

static void draw_char(int x, int y, int scale, u32 color, char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch < 32 || ch > 95)
        ch = ' ';
    const u8 *g = font5x7[ch - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1u << row))
                gfx_fill_rect(x + col * scale, y + row * scale,
                              scale, scale, color);
        }
    }
}

static int draw_text(int x, int y, int scale, u32 color, const char *text)
{
    int pen = x;
    while (*text) {
        if (*text != ' ')
            draw_char(pen, y, scale, color, *text);
        pen += 6 * scale;
        text++;
    }
    return pen;
}

static void gfx_screen(const char *reason, u32 vector, u32 error,
                       u32 eip, u32 cs, u32 eflags, const ExceptionFrame *f)
{
    int W = (int)gfx.width;
    int H = (int)gfx.height;
    u32 bg = 0x0A1020;
    u32 panel = 0x111C31;
    u32 red = 0xD1495B;
    u32 ink = 0xE8EEF9;
    u32 muted = 0x8A9AB8;

    gfx_clear(bg);
    gfx_fill_rect(0, 0, W, 8, red);
    gfx_fill_rect(0, H - 8, W, 8, red);

    int x = W / 12;
    if (x < 32) x = 32;

    draw_text(x, H / 10, 4, ink, "REVERTICALOS HAS STOPPED");
    draw_text(x, H / 10 + 48, 2, red, "KERNEL PANIC");

    int py = H / 10 + 96;
    int pw = W - x * 2;
    int ph = H - py - H / 8;
    if (ph < 200) ph = 200;
    gfx_fill_round(x, py, pw, ph, 12, panel, 255);


    int tx = x + 28;
    int ty = py + 34;
    int step = 30;

    draw_text(tx, ty, 2, muted, "REASON");
    draw_text(tx + 120, ty, 2, ink, reason);
    ty += step * 2;

    if (vector != 0xFFFFFFFFu) {
        char line[128];
        char *p = line;
        char *end = line + sizeof(line);
        p = append(p, end, "EXCEPTION #");
        p = append_dec(p, end, vector);
        p = append(p, end, "  ");
        p = append(p, end, exception_names[vector & 31]);
        draw_text(tx, ty, 2, ink, line);
        ty += step;

        p = line;
        p = append(p, end, "ERROR CODE  0x");
        p = append_hex(p, end, error, 8);
        draw_text(tx, ty, 2, ink, line);
        ty += step;

        p = line;
        p = append(p, end, "EIP  0x");
        p = append_hex(p, end, eip, 8);
        p = append(p, end, "    CS  0x");
        p = append_hex(p, end, cs, 4);
        draw_text(tx, ty, 2, ink, line);
        ty += step;

        p = line;
        p = append(p, end, "EFLAGS  0x");
        p = append_hex(p, end, eflags, 8);
        draw_text(tx, ty, 2, ink, line);
        ty += step * 2;

        if (f) {
            draw_text(tx, ty, 2, muted, "REGISTERS");
            ty += step;
            p = line;
            p = append(p, end, "EAX ");
            p = append_hex(p, end, f->eax, 8);
            p = append(p, end, "  EBX ");
            p = append_hex(p, end, f->ebx, 8);
            p = append(p, end, "  ECX ");
            p = append_hex(p, end, f->ecx, 8);
            draw_text(tx, ty, 2, ink, line);
            ty += step;
            p = line;
            p = append(p, end, "EDX ");
            p = append_hex(p, end, f->edx, 8);
            p = append(p, end, "  ESI ");
            p = append_hex(p, end, f->esi, 8);
            p = append(p, end, "  EDI ");
            p = append_hex(p, end, f->edi, 8);
            draw_text(tx, ty, 2, ink, line);
            ty += step;
            p = line;
            p = append(p, end, "EBP ");
            p = append_hex(p, end, f->ebp, 8);
            p = append(p, end, "  ESP ");
            p = append_hex(p, end, f->esp, 8);
            p = append(p, end, "  DS ");
            p = append_hex(p, end, f->ds, 4);
            draw_text(tx, ty, 2, ink, line);
        }
    }

    draw_text(x, H - H / 16, 2, muted,
              "SYSTEM HALTED - PRESS CTRL+ALT+DEL TO RESTART");
}

static void serial_panic(const char *reason, u32 vector, u32 error,
                         u32 eip, u32 cs, u32 eflags)
{
    serial_write("\r\n\r\n*** KERNEL PANIC ***\r\n");
    serial_write("reason: ");
    serial_write(reason ? reason : "(null)");
    serial_write("\r\n");
    if (vector != 0xFFFFFFFFu) {
        serial_write("vector: ");
        serial_dec(vector);
        serial_write(" ");
        serial_write(exception_names[vector & 31]);
        serial_write("\r\nerror:  ");
        serial_hex(error);
        serial_write("\r\neip:    ");
        serial_hex(eip);
        serial_write("\r\ncs:     ");
        serial_hex(cs);
        serial_write("\r\neflags: ");
        serial_hex(eflags);
        serial_write("\r\n");
    }
}

static void panic_common(const char *reason, u32 vector, u32 error,
                         u32 eip, u32 cs, u32 eflags,
                         const ExceptionFrame *f) __attribute__((noreturn));

static void panic_common(const char *reason, u32 vector, u32 error,
                         u32 eip, u32 cs, u32 eflags,
                         const ExceptionFrame *f)
{
    panic_common_begin:
    if (in_panic) {
        for (;;) {
            cli();
            hlt();
        }
    }
    in_panic = 1;
    cli();

    serial_panic(reason, vector, error, eip, cs, eflags);

    if (gfx.fb && gfx.back && gfx.width > 0 && gfx.height > 0)
        gfx_screen(reason, vector, error, eip, cs, eflags, f);
    else
        vga_screen(reason, vector, error, eip, cs, eflags);

    if (gfx.fb && gfx.back && gfx.width > 0 && gfx.height > 0)
        gfx_present();

    /* Keep polling Ctrl+Alt+Del so a physical machine can be restarted even
       though interrupts are off. */
    int ctrl = 0, alt = 0;
    for (;;) {
        cli();
        hlt();
        if (!(inb(0x64) & 0x01))
            continue;
        u8 sc = inb(0x60);
        if (sc == 0x1D) ctrl = 1;
        else if (sc == 0x9D) ctrl = 0;
        else if (sc == 0x38) alt = 1;
        else if (sc == 0xB8) alt = 0;
        else if (sc == 0x53 && ctrl && alt) {
            outb(0x64, 0xFE);
            outb(0xCF9, 0x06);
            goto panic_common_begin;
        }
    }
}

void panic(const char *reason)
{
    panic_common(reason ? reason : "unspecified kernel failure",
                 0xFFFFFFFFu, 0, 0, 0, 0, 0);
}

void panic_exception(const ExceptionFrame *f)
{
    u32 vector = f ? f->vector : 0xFFFFFFFFu;
    u32 error = f ? f->error : 0;
    u32 eip = f ? f->eip : 0;
    u32 cs = f ? f->cs : 0;
    u32 eflags = f ? f->eflags : 0;

    char reason[96];
    char *p = reason;
    char *end = reason + sizeof(reason);
    p = append(p, end, "unhandled CPU exception #");
    p = append_dec(p, end, vector);
    p = append(p, end, ": ");
    p = append(p, end, exception_names[vector & 31]);

    panic_common(reason, vector, error, eip, cs, eflags, f);
}
