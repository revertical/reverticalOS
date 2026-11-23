#ifndef RVOS_DISK_H
#define RVOS_DISK_H

#include "types.h"

#define DISK_MAX_PARTS 8

typedef struct {
    int dev;            /* index into the ATA drive table */
    int slot;           /* BIOS slot: 0 primary master, 1 primary slave, ... */
    u32 start_lba;
    u32 sectors;
    u8 type;
    int bootable;
} DiskPart;

void disk_init(void);
int disk_part_count(void);
const DiskPart *disk_part(int index);
u32 disk_size_bytes(int dev);
const char *disk_model(int dev);
int disk_slot(int dev);

int disk_read(int dev, u32 lba, u32 count, void *buf);
int disk_write(int dev, u32 lba, u32 count, const void *buf);

int disk_read_part(const DiskPart *p, u32 lba_offset, u32 count, void *buf);
int disk_write_part(const DiskPart *p, u32 lba_offset, u32 count,
                    const void *buf);

#endif
