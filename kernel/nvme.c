/* NVMe block driver.
 *
 * A compact polled driver: one admin queue pair and one I/O queue pair, with
 * 4 KiB PRP transfers.  The kernel is 32-bit and has no paging, so only BARs
 * below 4 GiB are usable; controllers whose register BAR sits above 4 GiB are
 * skipped instead of being accessed through a bogus address.
 */
#include "nvme.h"
#include "blk.h"
#include "pci.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

#define NVME_QUEUE_SIZE 64
#define NVME_ADMIN_QID  0
#define NVME_IO_QID     1

#define NVME_REG_CAP   0x0000
#define NVME_REG_VS    0x0008
#define NVME_REG_INTMS 0x000C
#define NVME_REG_INTMC 0x0010
#define NVME_REG_CC    0x0014
#define NVME_REG_CSTS  0x001C
#define NVME_REG_AQA   0x0024
#define NVME_REG_ASQ   0x0028
#define NVME_REG_ACQ   0x0030

#define NVME_CC_EN     0x00000001u
#define NVME_CC_CSS    0x00000070u
#define NVME_CC_MPS    0x00000780u
#define NVME_CC_IOSQES 0x000F0000u
#define NVME_CC_IOCQES 0x00F00000u
#define NVME_CSTS_RDY  0x00000001u
#define NVME_CSTS_CFS  0x00000002u

#define NVME_OP_WRITE  0x01
#define NVME_OP_READ   0x02
#define NVME_OP_FLUSH  0x00
#define NVME_OP_CREATE_SQ 0x01
#define NVME_OP_CREATE_CQ 0x05
#define NVME_OP_IDENTIFY  0x06

static volatile u8 *bar;
static int ready;
static u32 db_stride;
static u32 ns_sectors;
static char model[41];
static int cq_phase = 1;
static int sq_tail;
static int cq_head;
static int admin_tail;
static int admin_head;
static int admin_phase = 1;

static u8 admin_sq[NVME_QUEUE_SIZE * 64] __attribute__((aligned(4096)));
static u8 admin_cq[NVME_QUEUE_SIZE * 16] __attribute__((aligned(4096)));
static u8 io_sq[NVME_QUEUE_SIZE * 64] __attribute__((aligned(4096)));
static u8 io_cq[NVME_QUEUE_SIZE * 16] __attribute__((aligned(4096)));
static u8 io_buf[4096] __attribute__((aligned(4096)));
static u8 identify[4096] __attribute__((aligned(4096)));

static inline u32 r32(u32 off) { return *(volatile u32 *)(bar + off); }
static inline u64 r64(u32 off)
{
    u32 lo = *(volatile u32 *)(bar + off);
    u32 hi = *(volatile u32 *)(bar + off + 4);
    return ((u64)hi << 32) | lo;
}
static inline void w32(u32 off, u32 v) { *(volatile u32 *)(bar + off) = v; }
static inline void w64(u32 off, u64 v)
{
    *(volatile u32 *)(bar + off) = (u32)v;
    *(volatile u32 *)(bar + off + 4) = (u32)(v >> 32);
}
static inline void fence(void) { __asm__ volatile ("mfence" ::: "memory"); }

static int wait_csts(u32 mask, u32 value, u32 ms)
{
    u64 end = timer_ms() + ms;
    while (timer_ms() < end) {
        if ((r32(NVME_REG_CSTS) & mask) == value)
            return 0;
    }
    return -1;
}

static int cq_wait(u8 *cq, int *head, int *phase, u32 *result)
{
    u64 end = timer_ms() + 3000;
    while (timer_ms() < end) {
        u8 *e = cq + (u32)(*head) * 16;
        u32 dw3 = *(volatile u32 *)(e + 12);
        if ((int)(dw3 & 1) != *phase)
            continue;                       /* phase bit not flipped yet */

        u32 status = (dw3 >> 1) & 0x7FFF;
        if (result)
            *result = *(u32 *)e;
        *head = (*head + 1) % NVME_QUEUE_SIZE;
        if (*head == 0)
            *phase ^= 1;
        return (int)status;
    }
    return -1;
}

static void sq_submit(u8 *sq, int *tail, u32 qid, const u8 *cmd)
{
    memcpy(sq + (u32)(*tail) * 64, cmd, 64);
    fence();
    *tail = (*tail + 1) % NVME_QUEUE_SIZE;
    *(volatile u32 *)(bar + 0x1000 + (u32)(2 * qid) * db_stride) =
        (u32)*tail;
}

static int admin_cmd(const u8 *cmd)
{
    sq_submit(admin_sq, &admin_tail, NVME_ADMIN_QID, cmd);
    return cq_wait(admin_cq, &admin_head, &admin_phase, 0);
}

static int io_cmd(const u8 *cmd)
{
    sq_submit(io_sq, &sq_tail, NVME_IO_QID, cmd);
    return cq_wait(io_cq, &cq_head, &cq_phase, 0);
}

static void build_cmd(u8 *cmd, u8 opcode, u32 nsid, u32 cid,
                      u64 prp1, u32 cdw10, u32 cdw11, u32 cdw12)
{
    memset(cmd, 0, 64);
    cmd[0] = opcode;
    cmd[1] = 0;                 /* PSDT: PRP1 only */
    *(u16 *)(cmd + 2) = (u16)cid;
    *(u32 *)(cmd + 4) = nsid;
    *(u32 *)(cmd + 24) = (u32)prp1;
    *(u32 *)(cmd + 28) = (u32)(prp1 >> 32);
    *(u32 *)(cmd + 40) = cdw10;
    *(u32 *)(cmd + 44) = cdw11;
    *(u32 *)(cmd + 48) = cdw12;
}

static int init_queues(void)
{
    u8 cmd[64];

    /* Completion queue first, then submission queue. */
    build_cmd(cmd, NVME_OP_CREATE_CQ, 0, 1, 0,
              NVME_IO_QID | (NVME_QUEUE_SIZE << 16), 1, 0);
    if (admin_cmd(cmd) != 0)
        return -1;

    build_cmd(cmd, NVME_OP_CREATE_SQ, 0, 2, 0,
              NVME_IO_QID | (NVME_QUEUE_SIZE << 16), 1, 0);
    if (admin_cmd(cmd) != 0)
        return -1;

    return 0;
}

static int identify_ns(void)
{
    u8 cmd[64];
    build_cmd(cmd, NVME_OP_IDENTIFY, 1, 3, (u64)(uintptr_t)identify, 2, 0, 0);
    if (admin_cmd(cmd) != 0)
        return -1;

    u64 nsze = *(u64 *)(identify + 0);
    u32 flbas = identify[20] & 0x0F;
    u32 lbaf = *(u32 *)(identify + 128 + flbas * 4);
    u32 lbads = (lbaf >> 16) & 0xFF;
    if (lbads != 9) {
        serial_write("nvme: namespace is not 512-byte, skipping\r\n");
        return -1;
    }
    if (nsze == 0 || nsze > 0xFFFFFFFFu)
        nsze = 0xFFFFFFFFu;
    ns_sectors = (u32)nsze;
    return 0;
}

static int identify_ctrl(void)
{
    u8 cmd[64];
    build_cmd(cmd, NVME_OP_IDENTIFY, 0, 4, (u64)(uintptr_t)identify, 1, 0, 0);
    if (admin_cmd(cmd) != 0)
        return -1;

    const char *mn = (const char *)identify + 24;
    int n = 0;
    const char *p = "NVMe ";
    while (*p && n < 40) model[n++] = *p++;
    for (int i = 0; i < 40 && n < 40; i++) {
        char c = mn[i];
        if (c == 0)
            break;
        model[n++] = c;
    }
    while (n > 0 && model[n - 1] == ' ')
        n--;
    model[n] = 0;
    return 0;
}

void nvme_init(void)
{
    ready = 0;
    ns_sectors = 0;
    db_stride = 4;

    int bus, dev, func;
    if (!pci_find_class(0x01, 0x08, 0x02, &bus, &dev, &func)) {
        serial_write("nvme: no NVMe controller\r\n");
        return;
    }

    u32 lo = pci_read32(bus, dev, func, 0x10);
    u32 hi = pci_read32(bus, dev, func, 0x14);
    if ((lo & 1) || hi != 0) {
        serial_write("nvme: BAR is above 4 GiB, skipping\r\n");
        return;
    }
    u32 phys = lo & 0xFFFFFFF0u;
    if (!phys) {
        serial_write("nvme: unassigned BAR, skipping\r\n");
        return;
    }

    u16 pcicmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND, (u16)(pcicmd | 0x6));

    bar = (volatile u8 *)(uintptr_t)phys;
    serial_write("nvme: controller at ");
    serial_hex(phys);
    serial_write("\r\n");

    u64 cap = r64(NVME_REG_CAP);
    db_stride = 4u << ((cap >> 32) & 0x0F);
    u32 mqes = (u32)(cap & 0xFFFF) + 1;
    if (mqes < NVME_QUEUE_SIZE) {
        serial_write("nvme: queue size unsupported\r\n");
        return;
    }

    w32(NVME_REG_CC, 0);
    if (wait_csts(NVME_CSTS_RDY, 0, 2000) != 0) {
        serial_write("nvme: controller did not reset\r\n");
        return;
    }

    w32(NVME_REG_AQA, ((NVME_QUEUE_SIZE - 1) << 16) |
                       (NVME_QUEUE_SIZE - 1));
    w64(NVME_REG_ASQ, (u64)(uintptr_t)admin_sq);
    w64(NVME_REG_ACQ, (u64)(uintptr_t)admin_cq);

    u32 cc = NVME_CC_EN | (6u << 16) | (4u << 20);
    w32(NVME_REG_CC, cc);
    if (wait_csts(NVME_CSTS_RDY, NVME_CSTS_RDY, 3000) != 0) {
        serial_write("nvme: controller did not become ready\r\n");
        return;
    }
    if (r32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
        serial_write("nvme: controller fatal status\r\n");
        return;
    }

    if (identify_ctrl() != 0 || init_queues() != 0 || identify_ns() != 0) {
        serial_write("nvme: namespace setup failed\r\n");
        return;
    }

    if (blk_add(BLK_KIND_NVME, 0, -1, ns_sectors, model) < 0)
        return;

    ready = 1;
    serial_write("nvme: ");
    serial_write(model);
    serial_write(" ");
    serial_dec(ns_sectors / 2048);
    serial_write(" MB\r\n");
}

static int transfer(int index, u32 lba, u32 count, void *buf, int write)
{
    if (!ready || index != 0)
        return -1;
    if (!count || lba + count > ns_sectors)
        return -1;

    u8 *p = (u8 *)buf;
    while (count) {
        u32 chunk = count > 8 ? 8 : count;      /* 4 KiB per command */
        u32 bytes = chunk * 512u;
        if (write)
            memcpy(io_buf, p, bytes);

        u8 cmd[64];
        build_cmd(cmd, write ? NVME_OP_WRITE : NVME_OP_READ, 1, 5,
                  (u64)(uintptr_t)io_buf, lba, 0, chunk - 1);
        if (io_cmd(cmd) != 0)
            return -1;
        if (!write)
            memcpy(p, io_buf, bytes);

        lba += chunk;
        count -= chunk;
        p += bytes;
    }
    return 0;
}

int nvme_read(int index, u32 lba, u32 count, void *buf)
{
    return transfer(index, lba, count, buf, 0);
}

int nvme_write(int index, u32 lba, u32 count, const void *buf)
{
    return transfer(index, lba, count, (void *)buf, 1);
}

int nvme_flush(int index)
{
    if (!ready || index != 0)
        return -1;
    u8 cmd[64];
    build_cmd(cmd, NVME_OP_FLUSH, 1, 6, 0, 0, 0, 0);
    return io_cmd(cmd);
}
