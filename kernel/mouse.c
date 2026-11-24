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

static const char *mouse_name = "none";
static u32 aux_bytes;              /* total bytes seen on the aux port */
static u32 packets;                /* valid 3-byte motion packets parsed */
static u8 controller_cfg;          /* i8042 config byte read during init */
static int cfg_valid;              /* controller_cfg was read successfully */
static int reset_state;            /* 1 = device acknowledged 0xFF */
static u64 init_ms;                /* when mouse_init() last ran */
static int init_retries;
static int probe_pending;          /* liveness check done, device answered */
static int motion_logged;

#define PS2_STATUS_OBF  0x01       /* output buffer full */
#define PS2_STATUS_IBF  0x02       /* input buffer full */
#define PS2_STATUS_AUX  0x20       /* byte came from the auxiliary port */

#define PS2_ACK         0xFA
#define PS2_RESEND      0xFE

static int ps2_wait_input(u32 timeout_ms)
{
    u64 start = timer_ms();
    while ((inb(0x64) & PS2_STATUS_IBF) != 0) {
        if (timer_ms() - start >= timeout_ms)
            return 0;
    }
    return 1;
}

static int ps2_read(u8 *out, u32 timeout_ms)
{
    u64 start = timer_ms();
    for (;;) {
        if (inb(0x64) & PS2_STATUS_OBF) {
            *out = inb(0x60);
            return 1;
        }
        if (timer_ms() - start >= timeout_ms)
            return 0;
    }
}

/* Read a byte produced by the controller itself (for example its config
   byte).  Data bytes from the auxiliary port are not controller replies, so
   skip them instead of mistaking a motion packet for configuration. */
static int ps2_read_controller(u8 *out, u32 timeout_ms)
{
    u64 start = timer_ms();
    for (;;) {
        u8 status = inb(0x64);
        if (status & PS2_STATUS_OBF) {
            u8 value = inb(0x60);
            if (!(status & PS2_STATUS_AUX)) {
                *out = value;
                return 1;
            }
            continue;
        }
        if (timer_ms() - start >= timeout_ms)
            return 0;
    }
}

static int ps2_cmd(u8 cmd)
{
    if (!ps2_wait_input(50))
        return 0;
    outb(0x64, cmd);
    return 1;
}

static int ps2_write(u8 value)
{
    if (!ps2_wait_input(50))
        return 0;
    outb(0x60, value);
    return 1;
}

static void drain(void)
{
    for (int i = 0; i < 64; i++) {
        if ((inb(0x64) & PS2_STATUS_OBF) == 0)
            break;
        (void)inb(0x60);
    }
}

/* Send one byte to the auxiliary device and wait for its ACK.  A 0xFE means
   "resend"; anything else is stale data, so a failed command is reported to
   the caller instead of silently desynchronising the byte stream. */
static int mouse_cmd_timeout(u8 cmd, u32 timeout_ms)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        u8 reply;

        if (!ps2_cmd(0xD4) || !ps2_write(cmd))
            return 0;
        for (;;) {
            if (!ps2_read(&reply, timeout_ms))
                return 0;
            if (reply == PS2_ACK)
                return 1;
            if (reply == PS2_RESEND)
                break;
        }
    }
    return 0;
}

static int mouse_cmd(u8 cmd)
{
    return mouse_cmd_timeout(cmd, 50);
}

/* Commands with an argument (for example SETRES 0xE8 0x00) take a second
   byte that is forwarded to the device as well. */
static int mouse_cmd_arg(u8 cmd, u8 arg)
{
    return mouse_cmd(cmd) && mouse_cmd(arg);
}

static int mouse_getinfo(u8 out[3])
{
    if (!mouse_cmd(0xE9))
        return 0;
    return ps2_read(&out[0], 50) &&
           ps2_read(&out[1], 50) &&
           ps2_read(&out[2], 50);
}

/* A real reset (0xFF) is the only command that clears a packet format the
   firmware may have left behind.  "Set defaults" (0xF6) does not: a pad left
   in 4- or 6-byte mode keeps sending that format, and a 3-byte parser drops
   or misreads every packet.  That is what made the touchpad of a real Dell
   dead while emulated PS/2 mice worked. */
static int mouse_reset(void)
{
    u8 reply;
    int self_test = 0;

    if (!mouse_cmd_timeout(0xFF, 500))
        return 0;

    /* The device answers 0xFA, 0xAA (self-test passed), then its ID (0x00).
       Some pads omit the last bytes, so tolerate a short reply. */
    for (int i = 0; i < 4; i++) {
        if (!ps2_read(&reply, 500))
            break;
        if (reply == 0xAA) {
            self_test = 1;
            continue;
        }
        if (self_test)
            break;
    }
    timer_delay_ms(50);
    drain();
    packet_index = 0;
    return 1;
}

/* No vendor knocks here; see mouse_init(). */

void mouse_init(int start_x, int start_y, int mx, int my)
{
    mouse_x = start_x;
    mouse_y = start_y;
    max_x = mx;
    max_y = my;
    packet_index = 0;
    buttons_prev = 0;
    mouse_clicked_left = 0;
    init_ms = timer_ms();
    probe_pending = 0;

    drain();

    /* Enable the auxiliary (mouse) port. */
    ps2_cmd(0xA8);

    /* Controller config: aux and keyboard clocks on; the auxiliary IRQ stays
       off because this driver polls the output buffer. */
    u8 cfg = 0;
    if (ps2_cmd(0x20) && ps2_read_controller(&cfg, 50)) {
        controller_cfg = cfg;
        cfg_valid = 1;
        cfg &= (u8)~0x20;
        cfg &= (u8)~0x10;
        cfg &= (u8)~0x02;
        if (ps2_cmd(0x60))
            ps2_write(cfg);
    }
    timer_delay_ms(20);

    /* No vendor knocks: a relative cursor only needs the standard PS/2
       protocol, and vendor probes are how a pad ends up in a mode the 3-byte
       parser cannot read (the Elan knock F5,E6,E6,E6,E9 is also an ALPS "E6
       report", and Synaptics/ALPS extended modes change the packet format).
       A real reset puts the device in plain 3-byte relative mode; GETINFO is
       only used later as a liveness check and changes no state. */
    reset_state = mouse_reset();
    if (!reset_state) {
        /* A pad that does not acknowledge a reset is not going to answer the
           rest either.  Try the harmless commands anyway, then let the
           liveness retry in mouse_poll() attempt a full init later. */
        serial_write("mouse: reset not acknowledged\r\n");
        mouse_cmd(0xF6);
        mouse_cmd(0xEA);            /* stream mode: ALPS needs this */
        mouse_cmd(0xF4);
        drain();
        packet_index = 0;
        serial_write("mouse: PS/2 init incomplete\r\n");
        return;
    }

    mouse_name = "PS/2";
    serial_write("mouse: generic PS/2 mouse\r\n");

    /* Standard PS/2 mouse setup: 100 reports/s, 8 counts/mm, 1:1 scaling,
       stream mode, data reporting enabled.  SETSTREAM matters for ALPS:
       without it the pad accepts the commands but never reports. */
    mouse_cmd_arg(0xF3, 0x64);
    mouse_cmd_arg(0xE8, 0x03);
    mouse_cmd(0xE6);
    if (!mouse_cmd(0xEA))
        serial_write("mouse: stream mode not acknowledged\r\n");
    drain();
    packet_index = 0;
    if (!mouse_cmd(0xF4))
        serial_write("mouse: enable data reporting failed\r\n");
    drain();
    packet_index = 0;

    serial_write("mouse: PS/2 ready\r\n");
}

/* One-line state for the System Info probe and the on-screen diagnostics, so
   a machine without a serial port can still report what the controller and
   the pad did. */
static int put_text(char *out, int n, const char *s)
{
    while (*s && n < 63)
        out[n++] = *s++;
    return n;
}

static int put_u32(char *out, int n, u32 value)
{
    char digits[10];
    int d = 0;
    if (value == 0)
        digits[d++] = '0';
    while (value != 0) {
        digits[d++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (d != 0 && n < 63)
        out[n++] = digits[--d];
    return n;
}

static int put_hex8(char *out, int n, u8 value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (n < 62) out[n++] = hex[value >> 4];
    if (n < 62) out[n++] = hex[value & 0x0F];
    return n;
}

const char *mouse_status(void)
{
    static char text[64];
    int n = 0;

    n = put_text(text, n, mouse_name);
    n = put_text(text, n, " cfg=");
    if (cfg_valid)
        n = put_hex8(text, n, controller_cfg);
    else
        n = put_text(text, n, "??");
    n = put_text(text, n, reset_state ? " reset=ok" : " reset=FAIL");
    n = put_text(text, n, " ");
    n = put_u32(text, n, aux_bytes);
    n = put_text(text, n, "B ");
    n = put_u32(text, n, packets);
    n = put_text(text, n, "P");
    text[n] = 0;
    return text;
}

int mouse_has_motion(void)
{
    return packets != 0;
}

u32 mouse_byte_count(void)
{
    return aux_bytes;
}

u32 mouse_packet_count(void)
{
    return packets;
}

static void process_packet(void)
{
    u8 flags = packet[0];
    if (!(flags & 0x08)) {
        packet_index = 0;
        return;
    }

    if (!motion_logged) {
        motion_logged = 1;
        serial_write("mouse: first motion packet\r\n");
    }
    packets++;

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
        if (!(status & PS2_STATUS_OBF)) break;

        u8 data = inb(0x60);
        if (status & PS2_STATUS_AUX) {
            if (aux_bytes == 0)
                serial_write("mouse: first auxiliary byte\r\n");
            aux_bytes++;
            mouse_feed(data);
        } else {
            kbd_feed(data);
        }
    }

    /* An idle pad legitimately sends nothing until it is touched, so silence
       is not proof of failure.  Probe the device instead: if it answers
       GETINFO it is alive, and a pad that never answers gets at most two
       more initialisation attempts. */
    if (aux_bytes == 0 && init_retries < 2 && !probe_pending &&
        timer_ms() - init_ms > 2000) {
        u8 info[3];
        if (mouse_getinfo(info)) {
            probe_pending = 1;         /* alive: stop probing */
        } else {
            init_retries++;
            serial_write("mouse: device silent, retrying PS/2 init\r\n");
            mouse_init(mouse_x, mouse_y, max_x, max_y);
        }
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
