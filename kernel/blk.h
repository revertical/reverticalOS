#ifndef RVOS_BLK_H
#define RVOS_BLK_H

#include "types.h"

#define BLK_MAX 16
#define BLK_SECTOR 512u

enum {
    BLK_KIND_ATA = 0,
    BLK_KIND_AHCI,
    BLK_KIND_NVME,
    BLK_KIND_USB
};

typedef struct {
    int present;
    int kind;
    int index;          /* index inside the concrete driver */
    int slot;           /* BIOS slot for ATA, -1 otherwise */
    u32 sectors;        /* 512-byte sectors */
    const char *model;
} BlockDev;

void blk_init(void);
int  blk_count(void);
const BlockDev *blk_dev(int dev);

u32  blk_sectors(int dev);
const char *blk_model(int dev);
int  blk_slot(int dev);
int  blk_kind(int dev);

int  blk_read(int dev, u32 lba, u32 count, void *buf);
int  blk_write(int dev, u32 lba, u32 count, const void *buf);
int  blk_flush(int dev);

/* Drivers call this while probing. Returns the new device index or -1. */
int  blk_add(int kind, int index, int slot, u32 sectors, const char *model);

#endif
