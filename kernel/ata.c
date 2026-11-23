/* ATA PIO disk driver (LBA28).
 *
 * Polled programmed I/O on the legacy primary/secondary channels. This is the
 * smallest useful ATA driver: identify, read, write, flush. It intentionally
 * avoids interrupts and DMA, which keeps the kernel single-threaded and makes
 * the installer able to run while the desktop is live.
 */
#include "ata.h"
#include "io.h"
#include "serial.h"
#include "string.h"

#define REG_DATA      0
#define REG_ERROR     1
#define REG_FEATURES  1
#define REG_SECCOUNT  2
#define REG_LBA0      3
#define REG_LBA1      4
#define REG_LBA2      5
#define REG_DRIVE     6
#define REG_STATUS    7
#define REG_COMMAND   7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_DRDY 0x40
#define ST_BSY  0x80

#define CMD_READ      0x20
#define CMD_WRITE     0x30
#define CMD_FLUSH     0xE7
#define CMD_IDENTIFY  0xEC

static const u16 io_ports[2] = { 0x1F0, 0x170 };
static const u16 ctrl_ports[2] = { 0x3F6, 0x376 };

static AtaDrive drives[ATA_MAX_DRIVES];
static int drive_count;

static void delay400ns(u16 ctrl)
{
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
}

static int select_drive(const AtaDrive *d)
{
    outb(d->io_base + REG_DRIVE, (u8)(0xA0 | (d->slave ? 0x10 : 0x00)));
    delay400ns(d->ctrl_base);
    return 0;
}

static int wait_status(u16 io, u8 mask, u8 value)
{
    for (u32 i = 0; i < 2000000u; i++) {
        u8 st = inb(io + REG_STATUS);
        if (st == 0xFF)
            return -1;
        if (!(st & ST_BSY) && (st & mask) == value)
            return 0;
        if (st & (ST_ERR | ST_DF))
            return -1;
    }
    return -1;
}

static int wait_ready(u16 io)
{
    for (u32 i = 0; i < 2000000u; i++) {
        u8 st = inb(io + REG_STATUS);
        if (st == 0xFF)
            return -1;
        if (!(st & ST_BSY) && (st & ST_DRDY))
            return 0;
        if (st & (ST_ERR | ST_DF))
            return -1;
    }
    return -1;
}

static int identify(AtaDrive *d)
{
    select_drive(d);
    outb(d->io_base + REG_SECCOUNT, 0);
    outb(d->io_base + REG_LBA0, 0);
    outb(d->io_base + REG_LBA1, 0);
    outb(d->io_base + REG_LBA2, 0);
    outb(d->io_base + REG_COMMAND, CMD_IDENTIFY);

    delay400ns(d->ctrl_base);
    u8 st = inb(d->io_base + REG_STATUS);
    if (st == 0 || st == 0xFF)
        return 0;                       /* no device on this slot */
    if (wait_status(d->io_base, ST_DRQ, ST_DRQ) != 0)
        return 0;

    /* ATAPI (and SATA) devices answer IDENTIFY with a signature in the
       cylinder registers. Only plain ATA devices belong in this table. */
    u8 sig_lo = inb(d->io_base + REG_LBA1);
    u8 sig_hi = inb(d->io_base + REG_LBA2);
    if (sig_lo != 0 || sig_hi != 0)
        return 0;

    u16 buf[256];
    for (int i = 0; i < 256; i++)
        buf[i] = inw(d->io_base + REG_DATA);

    for (int i = 0; i < 40; i += 2) {
        d->model[i] = (char)(buf[27 + i / 2] >> 8);
        d->model[i + 1] = (char)(buf[27 + i / 2] & 0xFF);
    }
    if (buf[0] == 0 || buf[0] == 0xFFFF)
        return 0;
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = 0;

    d->sectors = (u32)buf[60] | ((u32)buf[61] << 16);
    if (d->sectors == 0 || d->sectors == 0xFFFFFFFFu)
        d->sectors = 0x0FFFFFFFu;
    d->present = 1;
    return 1;
}

void ata_init(void)
{
    drive_count = 0;
    memset(drives, 0, sizeof(drives));
    for (int ch = 0; ch < 2; ch++) {
        for (int slave = 0; slave < 2; slave++) {
            AtaDrive d;
            memset(&d, 0, sizeof(d));
            d.channel = ch;
            d.slave = slave;
            d.io_base = io_ports[ch];
            d.ctrl_base = ctrl_ports[ch];
            d.slot = ch * 2 + slave;
            if (identify(&d)) {
                drives[drive_count] = d;
                serial_write("ata: slot ");
                serial_dec(d.slot);
                serial_write(" ");
                serial_write(d.model);
                serial_write("  ");
                serial_dec(d.sectors / 2048);
                serial_write(" MB\r\n");
                drive_count++;
            }
        }
    }
}

int ata_count(void) { return drive_count; }

const AtaDrive *ata_drive(int index)
{
    if (index < 0 || index >= drive_count)
        return 0;
    return &drives[index];
}

int ata_slot(int index)
{
    if (index < 0 || index >= drive_count)
        return -1;
    return drives[index].slot;
}

static int pio_transfer(int index, u32 lba, u32 count, void *buf, int write)
{
    if (index < 0 || index >= drive_count || !buf)
        return -1;
    AtaDrive *d = &drives[index];
    if (count == 0)
        return 0;
    if (lba + count > d->sectors || lba > 0x0FFFFFFFu)
        return -1;

    u16 *p = (u16 *)buf;
    while (count > 0) {
        u32 n = count > 128 ? 128 : count;

        select_drive(d);
        if (wait_ready(d->io_base) != 0) {
            serial_write("ata: drive not ready\r\n");
            return -1;
        }

        outb(d->io_base + REG_SECCOUNT, (u8)(n & 0xFF));
        outb(d->io_base + REG_LBA0, (u8)(lba & 0xFF));
        outb(d->io_base + REG_LBA1, (u8)((lba >> 8) & 0xFF));
        outb(d->io_base + REG_LBA2, (u8)((lba >> 16) & 0xFF));
        outb(d->io_base + REG_DRIVE,
             (u8)(0xE0 | (d->slave ? 0x10 : 0) | ((lba >> 24) & 0x0F)));
        outb(d->io_base + REG_COMMAND, write ? CMD_WRITE : CMD_READ);

        for (u32 s = 0; s < n; s++) {
            if (wait_status(d->io_base, ST_DRQ, ST_DRQ) != 0) {
                serial_write("ata: transfer timeout\r\n");
                return -1;
            }
            if (write) {
                for (int i = 0; i < 256; i++)
                    outw(d->io_base + REG_DATA, *p++);
            } else {
                for (int i = 0; i < 256; i++)
                    *p++ = inw(d->io_base + REG_DATA);
            }
            delay400ns(d->ctrl_base);
        }

        lba += n;
        count -= n;
    }

    if (write)
        return ata_flush(index);
    return 0;
}

int ata_read(int index, u32 lba, u32 count, void *buf)
{
    return pio_transfer(index, lba, count, buf, 0);
}

int ata_write(int index, u32 lba, u32 count, const void *buf)
{
    return pio_transfer(index, lba, count, (void *)buf, 1);
}

int ata_flush(int index)
{
    if (index < 0 || index >= drive_count)
        return -1;
    AtaDrive *d = &drives[index];
    if (wait_ready(d->io_base) != 0)
        return -1;
    select_drive(d);
    outb(d->io_base + REG_COMMAND, CMD_FLUSH);
    return wait_ready(d->io_base);
}
