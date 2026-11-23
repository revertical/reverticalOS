#include "mouse.h"
#include "kbd.h"
#include "io.h"
#include "serial.h"
#include "timer.h"

int mouse_x;
int mouse_y;
u8 mouse_buttons;
u8 mouse_clicked_left;

static int max_x, max_y;
static u8 packet[3];
static int packet_index;
static u8 buttons_prev;

static int wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) return 1;
    }
    return 0;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return 1;
    }
    return 0;
}

static void mouse_write(u8 value)
{
    wait_write();
    outb(0x64, 0xD4);          /* next byte goes to the aux device */
    wait_write();
    outb(0x60, value);
    wait_read();
    (void)inb(0x60);           /* ACK */
}

static void drain(void)
{
    for (int i = 0; i < 64; i++) {
        if (inb(0x64) & 0x01)
            (void)inb(0x60);
        else
            break;
    }
}

static int mouse_read_byte(u8 *out)
{
    if (!wait_read())
        return 0;
    *out = inb(0x60);
    return 1;
}

/* Commands with an argument (for example SETRES 0xE8 0x00) need the data
   byte forwarded to the auxiliary device as well. */
static void mouse_cmd_arg(u8 cmd, u8 arg)
{
    mouse_write(cmd);
    mouse_write(arg);
}

static int mouse_getinfo(u8 out[3])
{
    mouse_write(0xE9);
    return mouse_read_byte(&out[0]) &&
           mouse_read_byte(&out[1]) &&
           mouse_read_byte(&out[2]);
}

/* Synaptics magic knock: four SETRES 0x00 commands, then GETINFO.  A real
   Synaptics pad answers with 0x47 in the second identification byte.  We
   keep it in relative mode (a touchpad must move the pointer, not jump it
   to an absolute position) and only ask for the high report rate. */
static int synaptics_detect(void)
{
    u8 r[3];
    for (int i = 0; i < 4; i++)
        mouse_cmd_arg(0xE8, 0x00);
    if (!mouse_getinfo(r))
        return 0;
    return r[1] == 0x47;
}

/* Synaptics mode byte via the "sliced" E6/E8 command syntax, followed by
   SETRATE 0x14 which commits the mode. 0x40 is relative high-rate mode. */
static void synaptics_mode(u8 mode)
{
    mouse_write(0xE6);
    for (int shift = 6; shift >= 0; shift -= 2)
        mouse_cmd_arg(0xE8, (u8)((mode >> shift) & 3));
    mouse_write(0xF3);
    mouse_write(0x14);
}

/* Elantech magic knock: reset/disable, three SETSCALE11 commands, GETINFO.
   A real Elantech pad answers 3C 03 C8 (or 3C 03 00).  We deliberately leave
   it in the mouse-compatible relative mode that every Elan pad supports. */
static int elantech_detect(void)
{
    u8 r[3];
    mouse_write(0xF6);          /* reset + disable reporting */
    timer_delay_ms(50);         /* let self-test bytes arrive */
    drain();
    mouse_write(0xF5);          /* disable */
    mouse_write(0xE6);
    mouse_write(0xE6);
    mouse_write(0xE6);
    if (!mouse_getinfo(r))
        return 0;
    return r[0] == 0x3C && r[1] == 0x03 &&
           (r[2] == 0xC8 || r[2] == 0x00);
}

void mouse_init(int start_x, int start_y, int mx, int my)
{
    mouse_x = start_x;
    mouse_y = start_y;
    max_x = mx;
    max_y = my;
    packet_index = 0;
    buttons_prev = 0;
    mouse_clicked_left = 0;

    drain();

    /* Enable the auxiliary (mouse) port. */
    wait_write();
    outb(0x64, 0xA8);

    /* Enable IRQ12, enable the aux clock. */
    wait_write();
    outb(0x64, 0x20);
    if (wait_read()) {
        u8 cfg = inb(0x60);
        cfg |= 0x02;
        cfg &= (u8)~0x20;
        wait_write();
        outb(0x64, 0x60);
        wait_write();
        outb(0x60, cfg);
    }

    /* Detect advanced touchpads, then always return to a known relative
       reporting mode.  Both kinds work as ordinary PS/2 mice even if the
       detection sequence is ignored. */
    mouse_write(0xF6);         /* defaults */
    timer_delay_ms(50);        /* reset answers 0xFA, 0xAA, 0x00 */
    drain();
    int synaptics = synaptics_detect();

    int elantech = 0;
    if (!synaptics) {
        mouse_write(0xF6);
        timer_delay_ms(50);
        drain();
        elantech = elantech_detect();
    }

    mouse_write(0xF6);         /* defaults again */
    timer_delay_ms(50);
    drain();
    if (synaptics) {
        synaptics_mode(0x40);  /* relative, high report rate */
        serial_write("mouse: Synaptics touchpad\r\n");
    } else if (elantech) {
        serial_write("mouse: Elan touchpad\r\n");
    }

    mouse_write(0xF3);         /* sample rate 100 */
    mouse_write(0x64);
    mouse_write(0xF4);         /* enable data reporting */
    drain();
    packet_index = 0;

    serial_write("mouse: PS/2 ready\r\n");
}

static void process_packet(void)
{
    u8 flags = packet[0];
    if (!(flags & 0x08)) {
        packet_index = 0;
        return;
    }

    int dx = packet[1];
    int dy = packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > max_x) mouse_x = max_x;
    if (mouse_y > max_y) mouse_y = max_y;

    u8 buttons = flags & 0x07;
    if ((buttons & 1) && !(buttons_prev & 1))
        mouse_clicked_left = 1;
    mouse_buttons = buttons;
    buttons_prev = buttons;
}

void mouse_feed(u8 data)
{
    if (packet_index == 0 && !(data & 0x08))
        return;
    packet[packet_index++] = data;
    if (packet_index == 3) {
        packet_index = 0;
        process_packet();
    }
}

void mouse_poll(void)
{
    for (int guard = 0; guard < 64; guard++) {
        u8 status = inb(0x64);
        if (!(status & 0x01)) break;

        u8 data = inb(0x60);
        if (status & 0x20)
            mouse_feed(data);
        else
            kbd_feed(data);
    }
}

/* Absolute pointer injection for USB touchscreens.  A touch is a left button
   press at the reported location; releasing produces a normal click state. */
void mouse_touch_absolute(int x, int y, int down)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    mouse_x = x;
    mouse_y = y;

    if (down) {
        if (!(buttons_prev & 1))
            mouse_clicked_left = 1;
        mouse_buttons = 1;
        buttons_prev = 1;
    } else {
        mouse_buttons = 0;
        buttons_prev = 0;
    }
}

/* Relative motion from a USB HID mouse.  HID Y grows downward, matching the
   screen; PS/2 packets are inverted by process_packet() instead. */
void mouse_usb_relative(int dx, int dy, u8 buttons)
{
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > max_x) mouse_x = max_x;
    if (mouse_y > max_y) mouse_y = max_y;

    u8 b = buttons & 0x07;
    if ((b & 1) && !(buttons_prev & 1))
        mouse_clicked_left = 1;
    mouse_buttons = b;
    buttons_prev = b;
}
