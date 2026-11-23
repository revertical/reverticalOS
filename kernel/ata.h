#ifndef RVOS_ATA_H
#define RVOS_ATA_H

#include "types.h"

#define ATA_MAX_DRIVES 4

typedef struct {
    int present;
    int channel;        /* 0 = primary, 1 = secondary */
    int slave;
    int slot;           /* BIOS slot: 0 primary master, 1 primary slave, ... */
    u16 io_base;
    u16 ctrl_base;
    u32 sectors;        /* LBA28 capacity */
    char model[41];
} AtaDrive;

void ata_init(void);
int  ata_count(void);
const AtaDrive *ata_drive(int index);
int  ata_slot(int index);
int  ata_read(int index, u32 lba, u32 count, void *buf);
int  ata_write(int index, u32 lba, u32 count, const void *buf);
int  ata_flush(int index);

#endif
