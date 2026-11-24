#ifndef RVOS_MOUSE_H
#define RVOS_MOUSE_H

#include "types.h"

extern int mouse_x;
extern int mouse_y;
extern u8 mouse_buttons;
extern u8 mouse_clicked_left;

void mouse_init(int start_x, int start_y, int max_x, int max_y);
void mouse_poll(void);
void mouse_feed(u8 data);

/* One-line PS/2 diagnostic ("Alps cfg=47 reset=ok 812B 97P") for System Info
   and the on-screen input diagnostics panel. */
const char *mouse_status(void);
int mouse_has_motion(void);        /* at least one valid packet parsed */
u32 mouse_byte_count(void);        /* bytes seen on the auxiliary port */
u32 mouse_packet_count(void);      /* valid motion packets parsed */

/* Input paths used by the USB HID driver.  Relative motion uses the same
   button bit layout as PS/2 (bit 0 left, bit 1 right, bit 2 middle). */
void mouse_usb_relative(int dx, int dy, u8 buttons);
void mouse_touch_absolute(int x, int y, int down);

#endif
