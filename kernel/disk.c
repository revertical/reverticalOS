/* Disk layer: block devices and their MBR partitions. */
#include "disk.h"
#include "blk.h"
#include "serial.h"
#include "string.h"

#define MBR_SIGNATURE 0xAA55u

static DiskPart parts[DISK_MAX_PARTS];
static int part_count;

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void scan_disk(int dev)
{
    u8 sector[512];
    if (disk_read(dev, 0, 1, sector) != 0) {
        serial_write("disk: read error on drive\r\n");
        return;
    }
    if (le16(sector + 510) != MBR_SIGNATURE) {
        serial_write("disk: no MBR on drive ");
        serial_dec(dev);
        serial_write("\r\n");
        return;
    }

    u32 total = blk_sectors(dev);
    for (int i = 0; i < 4 && part_count < DISK_MAX_PARTS; i++) {
        const u8 *e = sector + 446 + i * 16;
        u8 type = e[4];
        u32 start = le32(e + 8);
        u32 count = le32(e + 12);
        if (type == 0 || count == 0)
            continue;
        if (start == 0 || start >= total || count > total - start)
            continue;
        DiskPart *p = &parts[part_count++];
        p->dev = dev;
        p->slot = blk_slot(dev);
        p->start_lba = start;
        p->sectors = count;
        p->type = type;
        p->bootable = (e[0] & 0x80) != 0;
    }
}

void disk_init(void)
{
    part_count = 0;
    memset(parts, 0, sizeof(parts));
    for (int i = 0; i < blk_count(); i++)
        scan_disk(i);

    serial_write("disk: ");
    serial_dec(blk_count());
    serial_write(" drive(s), ");
    serial_dec(part_count);
    serial_write(" partition(s)\r\n");
}

int disk_part_count(void) { return part_count; }

const DiskPart *disk_part(int index)
{
    if (index < 0 || index >= part_count)
        return 0;
    return &parts[index];
}

u32 disk_size_bytes(int dev)
{
    return blk_sectors(dev) * 512u;
}

const char *disk_model(int dev)
{
    return blk_model(dev);
}

int disk_slot(int dev)
{
    return blk_slot(dev);
}

int disk_read(int dev, u32 lba, u32 count, void *buf)
{
    return blk_read(dev, lba, count, buf);
}

int disk_write(int dev, u32 lba, u32 count, const void *buf)
{
    return blk_write(dev, lba, count, buf);
}

int disk_read_part(const DiskPart *p, u32 lba_offset, u32 count, void *buf)
{
    if (!p)
        return -1;
    if (lba_offset + count > p->sectors)
        return -1;
    return blk_read(p->dev, p->start_lba + lba_offset, count, buf);
}

int disk_write_part(const DiskPart *p, u32 lba_offset, u32 count,
                    const void *buf)
{
    if (!p)
        return -1;
    if (lba_offset + count > p->sectors)
        return -1;
    return blk_write(p->dev, p->start_lba + lba_offset, count, buf);
}
