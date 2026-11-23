#ifndef RVOS_USB_H
#define RVOS_USB_H

#include "types.h"

void usb_init(void);
void usb_poll(void);
int  usb_present(void);

#endif
