/* AHCI (SATA) block driver.
 *
 * Polled DMA on the first AHCI controller.  This is the storage path used by
 * most real x86 machines that no longer expose legacy IDE ports.  One command
 * list/FIS area per implemented port, one shared 64 KiB bounce buffer because
 * the kernel is single threaded.
 */
#include "ahci.h"
#include "blk.h"
#include "pci.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

#define AHCI_MAX_PORTS 8
#define HBA_PORT_BASE  0x100
#define HBA_PORT_SIZE  0x80

#define HBA_GHC_AE     0x80000000u
#define HBA_GHC_HR     0x00000001u

#define PORT_CMD_ST     0x00000001u
#define PORT_CMD_FRE    0x00000010u
#define PORT_CMD_FR     0x00004000u
#define PORT_CMD_CR     0x00008000u
#define PORT_CMD_POD    0x00000002u
#define PORT_CMD_SUD    0x00000004u
#define PORT_CMD_ICC    0x0000000Fu

#define PORT_IS_TFES    0x40000000u
#define PORT_IS_HBFS    0x10000000u
#define PORT_IS_IFS     0x08000000u
#define PORT_IS_ERR     0x04000000u

#define CMD_READ_DMA_EXT  0x25
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_IDENTIFY      0xEC
#define CMD_FLUSH_EXT     0xEA

typedef struct {
    int present;
    int port;
    u32 sectors;
    char model[41];
    volatile u8 *regs;
} AhciPort;

static AhciPort ports[AHCI_MAX_PORTS];
static int port_count;
static volatile u8 *abar;
static int controller_ready;

static u8 cmd_list[AHCI_MAX_PORTS][1024] __attribute__((aligned(1024)));
static u8 fis_area[AHCI_MAX_PORTS][256] __attribute__((aligned(256)));
static u8 cmd_table[AHCI_MAX_PORTS][256] __attribute__((aligned(128)));
static u8 bounce[65536] __attribute__((aligned(4096)));

static inline u32 r32(volatile u8 *p, u32 off)
{
    return *(volatile u32 *)(p + off);
}

static inline void w32(volatile u8 *p, u32 off, u32 v)
{
    *(volatile u32 *)(p + off) = v;
}

static int bar32(int bus, int dev, int func, int off)
{
    u32 v = pci_read32(bus, dev, func, off);
    if (v & 1)
        return 0;                            /* I/O BAR, not usable here */
    if (((v >> 1) & 3) == 2) {
        u32 hi = pci_read32(bus, dev, func, off + 4);
        if (hi != 0)
            return 0;                        /* above 4 GiB, no paging */
    }
    u32 addr = v & 0xFFFFFFF0u;
    if (addr == 0)
        return 0;
    return (int)addr;
}

static void wait_until(u32 *reg, u32 mask, u32 value, u32 ms)
{
    u64 end = timer_ms() + ms;
    while (timer_ms() < end) {
        if ((*reg & mask) == value)
            return;
    }
}

static int port_start(AhciPort *p)
{
    volatile u8 *r = p->regs;

    u32 cmd = r32(r, 0x18);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    w32(r, 0x18, cmd);
    wait_until((u32 *)(r + 0x18), PORT_CMD_CR | PORT_CMD_FR, 0, 500);

    w32(r, 0x00, (u32)(uintptr_t)cmd_list[p - ports]);
    w32(r, 0x04, 0);
    w32(r, 0x08, (u32)(uintptr_t)fis_area[p - ports]);
    w32(r, 0x0C, 0);

    w32(r, 0x10, 0xFFFFFFFFu);              /* clear pending interrupts */
    w32(r, 0x30, 0xFFFFFFFFu);              /* clear errors */

    cmd = r32(r, 0x18);
    cmd |= PORT_CMD_FRE | PORT_CMD_POD | PORT_CMD_SUD;
    cmd = (cmd & ~PORT_CMD_ICC) | 0x01;     /* interface active */
    w32(r, 0x18, cmd);
    cmd |= PORT_CMD_ST;
    w32(r, 0x18, cmd);

    u64 end = timer_ms() + 1000;
    while (timer_ms() < end) {
        u32 ssts = r32(r, 0x28);
        if ((ssts & 0x0F) == 3 && ((ssts >> 8) & 0x0F) == 1)
            return 0;
    }
    return -1;
}

static int issue(AhciPort *p, u8 command, u64 lba, u16 count,
                 void *buf, int write)
{
    int slot = p - ports;
    volatile u8 *r = p->regs;
    u8 *ct = cmd_table[slot];
    u8 *cfis = ct;
    u8 *prdt = ct + 0x80;

    if (count > 128)
        return -1;

    u32 bytes = (u32)count * 512u;
    if (bytes) {
        if (write)
            memcpy(bounce, buf, bytes);
        memset(prdt, 0, 16);
        w32(prdt, 0x00, (u32)(uintptr_t)bounce);
        w32(prdt, 0x04, 0);
        w32(prdt, 0x08, bytes - 1);
    } else {
        memset(prdt, 0, 16);
    }

    memset(cfis, 0, 64);
    cfis[0] = 0x27;
    cfis[1] = 0x80;
    cfis[2] = command;
    cfis[4] = (u8)(lba & 0xFF);
    cfis[5] = (u8)((lba >> 8) & 0xFF);
    cfis[6] = (u8)((lba >> 16) & 0xFF);
    cfis[7] = 0x40;                         /* LBA mode */
    cfis[8] = (u8)((lba >> 24) & 0xFF);
    cfis[9] = (u8)((lba >> 32) & 0xFF);
    cfis[10] = (u8)((lba >> 40) & 0xFF);
    cfis[12] = (u8)(count & 0xFF);
    cfis[13] = (u8)((count >> 8) & 0xFF);

    u8 *hdr = cmd_list[slot];
    memset(hdr, 0, 32);
    hdr[0] = (u8)(5 | (write ? 0x40 : 0));  /* CFL + W */
    *(u16 *)(hdr + 4) = (u16)(bytes ? 1 : 0); /* PRDTL */
    *(u32 *)(hdr + 8) = (u32)(uintptr_t)ct;  /* CTBA */
    *(u32 *)(hdr + 12) = 0;

    u32 is = r32(r, 0x10);
    w32(r, 0x10, is | PORT_IS_ERR | PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_IFS);

    u32 ci = r32(r, 0x38);
    w32(r, 0x38, ci | (1u << slot));

    u64 end = timer_ms() + 3000;
    while (timer_ms() < end) {
        u32 pending = r32(r, 0x38) & (1u << slot);
        u32 status = r32(r, 0x10);
        if (status & (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_IFS | PORT_IS_ERR))
            return -1;
        if (!pending) {
            if (bytes && !write)
                memcpy(buf, bounce, bytes);
            return 0;
        }
    }
    return -1;
}

static void identify(AhciPort *p)
{
    u16 id[256];
    if (issue(p, CMD_IDENTIFY, 0, 1, id, 0) != 0) {
        serial_write("ahci: identify failed on port ");
        serial_dec(p->port);
        serial_write("\r\n");
        return;
    }

    for (int i = 0; i < 40; i += 2) {
        p->model[i] = (char)(id[27 + i / 2] >> 8);
        p->model[i + 1] = (char)(id[27 + i / 2] & 0xFF);
    }
    p->model[40] = 0;
    for (int i = 39; i >= 0 && p->model[i] == ' '; i--)
        p->model[i] = 0;

    u32 lba28 = (u32)id[60] | ((u32)id[61] << 16);
    u32 lba48 = (u32)id[100] | ((u32)id[101] << 16);
    p->sectors = lba48 ? lba48 : lba28;
    if (p->sectors == 0 || p->sectors == 0xFFFFFFFFu)
        p->sectors = 0x0FFFFFFFu;
}

void ahci_init(void)
{
    port_count = 0;
    controller_ready = 0;
    memset(ports, 0, sizeof(ports));

    int bus, dev, func;
    if (!pci_find_class(0x01, 0x06, 0x01, &bus, &dev, &func)) {
        serial_write("ahci: no AHCI controller\r\n");
        return;
    }

    u32 bar = (u32)bar32(bus, dev, func, 0x24);
    if (!bar) {
        serial_write("ahci: unusable BAR5\r\n");
        return;
    }

    u16 cmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND, (u16)(cmd | 0x6));

    abar = (volatile u8 *)(uintptr_t)bar;
    serial_write("ahci: controller at ");
    serial_hex(bar);
    serial_write("\r\n");

    u32 cap = r32(abar, 0x00);
    u32 pi = r32(abar, 0x0C);
    int max_ports = (int)(cap & 0x1F) + 1;
    if (max_ports > AHCI_MAX_PORTS)
        max_ports = AHCI_MAX_PORTS;

    w32(abar, 0x04, HBA_GHC_AE | HBA_GHC_HR);
    wait_until((u32 *)(abar + 0x04), HBA_GHC_HR, 0, 1000);
    w32(abar, 0x04, HBA_GHC_AE);

    for (int port = 0; port < max_ports; port++) {
        if (!(pi & (1u << port)))
            continue;
        volatile u8 *r = abar + HBA_PORT_BASE + port * HBA_PORT_SIZE;
        u32 sig = r32(r, 0x24);
        if (sig != 0x00000101u)
            continue;

        AhciPort *p = &ports[port_count];
        p->present = 1;
        p->port = port;
        p->regs = r;
        if (port_start(p) != 0) {
            serial_write("ahci: port ");
            serial_dec(port);
            serial_write(" did not come ready\r\n");
            p->present = 0;
            continue;
        }
        identify(p);
        if (p->sectors == 0) {
            p->present = 0;
            continue;
        }

        int dev_index = blk_add(BLK_KIND_AHCI, port_count, -1,
                                p->sectors, p->model);
        if (dev_index < 0) {
            p->present = 0;
            continue;
        }

        serial_write("ahci: port ");
        serial_dec(port);
        serial_write(" ");
        serial_write(p->model);
        serial_write("\r\n");
        port_count++;
    }
    controller_ready = port_count > 0;
}

int ahci_read(int index, u32 lba, u32 count, void *buf)
{
    if (index < 0 || index >= port_count || !ports[index].present)
        return -1;
    AhciPort *p = &ports[index];
    u8 *out = (u8 *)buf;
    while (count) {
        u32 chunk = count > 128 ? 128 : count;
        if (lba + chunk > p->sectors)
            return -1;
        if (issue(p, CMD_READ_DMA_EXT, lba, (u16)chunk, out, 0) != 0)
            return -1;
        lba += chunk;
        count -= chunk;
        out += chunk * 512u;
    }
    return 0;
}

int ahci_write(int index, u32 lba, u32 count, const void *buf)
{
    if (index < 0 || index >= port_count || !ports[index].present)
        return -1;
    AhciPort *p = &ports[index];
    const u8 *in = (const u8 *)buf;
    while (count) {
        u32 chunk = count > 128 ? 128 : count;
        if (lba + chunk > p->sectors)
            return -1;
        if (issue(p, CMD_WRITE_DMA_EXT, lba, (u16)chunk, (void *)in, 1) != 0)
            return -1;
        lba += chunk;
        count -= chunk;
        in += chunk * 512u;
    }
    return 0;
}

int ahci_flush(int index)
{
    if (index < 0 || index >= port_count || !ports[index].present)
        return -1;
    return issue(&ports[index], CMD_FLUSH_EXT, 0, 0, 0, 0);
}
