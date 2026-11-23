#ifndef RVOS_POWER_H
#define RVOS_POWER_H

#include "types.h"

/* Best-effort ACPI power management. All of it is optional: on a machine
   without usable ACPI tables the UI simply offers a safe-to-switch-off
   screen instead. */

/* acpi_rsdp: RSDP physical address supplied by the firmware (UEFI) or 0 to
   scan the BIOS memory areas. */
void power_init(u64 acpi_rsdp);
int  power_available(void);

/* Try to power the machine off. Returns only if the request did not work. */
int  power_shutdown(void);

/* Reset the machine; never returns. */
void power_reboot(void) __attribute__((noreturn));

#endif
