#ifndef RVOS_AHCI_H
#define RVOS_AHCI_H

#include "types.h"

void ahci_init(void);
int  ahci_read(int index, u32 lba, u32 count, void *buf);
int  ahci_write(int index, u32 lba, u32 count, const void *buf);
int  ahci_flush(int index);

#endif
