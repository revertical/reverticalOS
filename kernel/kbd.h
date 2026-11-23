#ifndef RVOS_KBD_H
#define RVOS_KBD_H

#include "types.h"

/* Special keys are reported above the ASCII range. */
#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108
#define KEY_DEL    0x109
#define KEY_ESC    0x1B
#define KEY_ENTER  '\n'
#define KEY_BACK   '\b'
#define KEY_TAB    '\t'

void kbd_init(void);
void kbd_feed(u8 scancode);
int  kbd_getc(void);          /* 0 when the queue is empty */
int  kbd_ctrl(void);

#endif
