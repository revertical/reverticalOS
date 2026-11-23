/* Unified 512-byte block device table.
 *
 * ATA PIO, AHCI and NVMe all register here.  The partition layer, RVFS and
 * the installer only ever see BlockDev entries, which is what lets the same
 * system boot from legacy IDE, SATA/AHCI and NVMe hardware.
 */
#include "blk.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "serial.h"
#include "string.h"

static BlockDev devs[BLK_MAX];
static int dev_count;

int blk_add(int kind, int index, int slot, u32 sectors, const char *model)
{
    if (dev_count >= BLK_MAX || sectors == 0)
        return -1;
    BlockDev *d = &devs[dev_count];
    d->present = 1;
    d->kind = kind;
    d->index = index;
    d->slot = slot;
    d->sectors = sectors;
    d->model = model ? model : "";
    return dev_count++;
}

void blk_init(void)
{
    dev_count = 0;
    memset(devs, 0, sizeof(devs));

    ata_init();
    for (int i = 0; i < ata_count(); i++) {
        const AtaDrive *d = ata_drive(i);
        if (d && d->present)
            blk_add(BLK_KIND_ATA, i, ata_slot(i), d->sectors, d->model);
    }

    ahci_init();
    nvme_init();

    serial_write("blk: ");
    serial_dec(dev_count);
    serial_write(" block device(s)\r\n");
    for (int i = 0; i < dev_count; i++) {
        serial_write("blk: #");
        serial_dec(i);
        serial_write(" ");
        serial_write(devs[i].model);
        serial_write(" ");
        serial_dec(devs[i].sectors / 2048);
        serial_write(" MB");
        if (devs[i].kind == BLK_KIND_NVME)
            serial_write(" nvme");
        else if (devs[i].kind == BLK_KIND_AHCI)
            serial_write(" ahci");
        else if (devs[i].kind == BLK_KIND_USB)
            serial_write(" usb");
        serial_write("\r\n");
    }
}

int blk_count(void) { return dev_count; }

const BlockDev *blk_dev(int dev)
{
    if (dev < 0 || dev >= dev_count)
        return 0;
    return &devs[dev];
}

u32 blk_sectors(int dev)
{
    const BlockDev *d = blk_dev(dev);
    return d ? d->sectors : 0;
}

const char *blk_model(int dev)
{
    const BlockDev *d = blk_dev(dev);
    return d ? d->model : "";
}

int blk_slot(int dev)
{
    const BlockDev *d = blk_dev(dev);
    return d ? d->slot : -1;
}

int blk_kind(int dev)
{
    const BlockDev *d = blk_dev(dev);
    return d ? d->kind : -1;
}

int blk_read(int dev, u32 lba, u32 count, void *buf)
{
    const BlockDev *d = blk_dev(dev);
    if (!d || count == 0 || lba + count > d->sectors)
        return -1;
    switch (d->kind) {
    case BLK_KIND_ATA:  return ata_read(d->index, lba, count, buf);
    case BLK_KIND_AHCI: return ahci_read(d->index, lba, count, buf);
    case BLK_KIND_NVME: return nvme_read(d->index, lba, count, buf);
    default: return -1;
    }
}

int blk_write(int dev, u32 lba, u32 count, const void *buf)
{
    const BlockDev *d = blk_dev(dev);
    if (!d || count == 0 || lba + count > d->sectors)
        return -1;
    switch (d->kind) {
    case BLK_KIND_ATA:  return ata_write(d->index, lba, count, buf);
    case BLK_KIND_AHCI: return ahci_write(d->index, lba, count, buf);
    case BLK_KIND_NVME: return nvme_write(d->index, lba, count, buf);
    default: return -1;
    }
}

int blk_flush(int dev)
{
    const BlockDev *d = blk_dev(dev);
    if (!d)
        return -1;
    switch (d->kind) {
    case BLK_KIND_ATA:  return ata_flush(d->index);
    case BLK_KIND_AHCI: return ahci_flush(d->index);
    case BLK_KIND_NVME: return nvme_flush(d->index);
    default: return 0;
    }
}
