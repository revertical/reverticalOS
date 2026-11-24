/* PS/2 keyboard, polled.
 *
 * The i8042 output buffer is shared by the keyboard and the auxiliary
 * (mouse) port, so kernel/mouse.c drains port 0x60 and hands every byte to
 * either mouse_feed() or kbd_feed() depending on the status register.
 */

#include "kbd.h"
#include "io.h"
#include "serial.h"

#define QUEUE_SIZE 64

static u16 queue[QUEUE_SIZE];
static int qhead, qtail;
static int shift_down;
static int caps_down;
static int ctrl_down;
static int extended;

/* US layout, scancode set 1. 0 means "no printable character". */
static const char map_normal[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const char map_shift[128] = {
    0,   0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static void push(u16 value)
{
    int next = (qhead + 1) % QUEUE_SIZE;
    if (next == qtail)
        return;                    /* drop when full */
    queue[qhead] = value;
    qhead = next;
}

int kbd_ctrl(void) { return ctrl_down; }

static int wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & 0x02) == 0) return 1;
    return 0;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) return 1;
    return 0;
}

static void kbd_write(u8 value)
{
    if (!wait_write()) return;
    outb(0x60, value);
}

void kbd_init(void)
{
    qhead = qtail = 0;
    shift_down = caps_down = ctrl_down = extended = 0;

    /* Flush anything the firmware left behind. */
    for (int i = 0; i < 64 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    /* Controller config: keyboard clock on, keyboard IRQ enabled. */
    if (wait_write()) outb(0x64, 0x20);
    if (wait_read()) {
        u8 cfg = inb(0x60);
        cfg |= 0x01;
        cfg &= (u8)~0x10;
        if (wait_write()) outb(0x64, 0x60);
        if (wait_write()) outb(0x60, cfg);
    }

    if (wait_write()) outb(0x64, 0xAE);   /* enable the keyboard port */
    kbd_write(0xF4);                      /* enable scanning */
    if (wait_read()) (void)inb(0x60);

    serial_write("kbd: PS/2 ready\r\n");
}

void kbd_feed(u8 sc)
{
    if (sc == 0xE0) { extended = 1; return; }

    if (sc & 0x80) {
        u8 code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift_down = 0;
        if (code == 0x1D) ctrl_down = 0;
        extended = 0;
        return;
    }

    if (extended) {
        extended = 0;
        if (sc == 0x48) push(KEY_UP);
        else if (sc == 0x50) push(KEY_DOWN);
        else if (sc == 0x4B) push(KEY_LEFT);
        else if (sc == 0x4D) push(KEY_RIGHT);
        else if (sc == 0x47) push(KEY_HOME);
        else if (sc == 0x4F) push(KEY_END);
        else if (sc == 0x49) push(KEY_PGUP);
        else if (sc == 0x51) push(KEY_PGDN);
        else if (sc == 0x53) push(KEY_DEL);
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0x3A) { caps_down = !caps_down; return; }
    if (sc == 0x1D) { ctrl_down = 1; return; }
    if (sc == 0x3B) { push(KEY_F1); return; }

    char c = shift_down ? map_shift[sc] : map_normal[sc];
    if (!c) return;
    if (caps_down && !shift_down && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (ctrl_down) {
        char low = c;
        if (low >= 'A' && low <= 'Z') low = (char)(low - 'A' + 'a');
        if (low >= 'a' && low <= 'z') c = (char)(low - 'a' + 1);
    }
    push((u16)(u8)c);
}

int kbd_getc(void)
{
    while (qhead != qtail) {
        u16 value = queue[qtail];
        qtail = (qtail + 1) % QUEUE_SIZE;
        if (value)
            return (int)value;
    }
    return 0;
}
