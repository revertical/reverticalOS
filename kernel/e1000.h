#ifndef RVOS_E1000_H
#define RVOS_E1000_H

#include "types.h"

/* Intel PRO/1000 (82540EM and friends).  The driver registers an ethernet
   NetIf named "eth0" and polls the receive ring from net_poll(). */
int e1000_init(void);
int e1000_present(void);
void e1000_poll(void);

#endif
