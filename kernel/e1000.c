/* Intel 8254x (PRO/1000) ethernet driver - polling mode.
 *
 * VirtualBox's default adapter is the 82540EM, so this is the one that makes
 * the VM's NAT network reachable.  Transmit and receive use the classic
 * legacy descriptor rings; interrupts stay masked and net_poll() drains the
 * receive ring once per desktop frame.
 */

#include "e1000.h"
#include "net.h"
#include "pci.h"
#include "io.h"
#include "timer.h"
#include "serial.h"
#include "string.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EECD   0x0010
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_RST   0x04000000u
#define CTRL_SLU   0x00000040u
#define CTRL_ASDE  0x00000020u

#define RCTL_EN    0x00000002u
#define RCTL_SBP   0x00000004u
#define RCTL_UPE   0x00000008u
#define RCTL_MPE   0x00000010u
#define RCTL_BAM   0x00008000u
#define RCTL_SECRC 0x04000000u

#define TCTL_EN    0x00000002u
#define TCTL_PSP   0x00000008u
#define TCTL_CT    0x00000100u

#define TX_COUNT 8
#define RX_COUNT 16
#define RX_SIZE  2048

typedef struct {
    u64 addr;
    u16 len;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) E1000Desc;

static volatile u32 *mmio;
static int present;
static int tx_index, rx_index;
static u8 mac[6];

static E1000Desc tx_ring[TX_COUNT] __attribute__((aligned(16)));
static E1000Desc rx_ring[RX_COUNT] __attribute__((aligned(16)));
static u8 tx_buf[TX_COUNT][RX_SIZE] __attribute__((aligned(16)));
static u8 rx_buf[RX_COUNT][RX_SIZE] __attribute__((aligned(16)));

static NetIf eth;

static inline void reg_write(u32 reg, u32 value)
{
    mmio[reg / 4] = value;
    __asm__ volatile ("" ::: "memory");
}

static inline u32 reg_read(u32 reg)
{
    u32 value = mmio[reg / 4];
    __asm__ volatile ("" ::: "memory");
    return value;
}

static u16 eeprom_read(int address)
{
    reg_write(REG_EERD, ((u32)(address & 0xFF) << 8) | 1u);
    for (int i = 0; i < 100000; i++) {
        u32 v = reg_read(REG_EERD);
        if (v & 0x10u)
            return (u16)(v >> 16);
    }
    return 0;
}

static int e1000_send(NetIf *ifp, const void *frame, u32 len)
{
    (void)ifp;
    if (!present || len > RX_SIZE)
        return -1;
    int i = tx_index;
    int guard = 0;
    /* A set DD bit means the descriptor is free again; clear it before
       handing the slot back to the hardware. */
    for (; guard < 1000000; guard++) {
        if (tx_ring[i].status & 0x01u)
            break;
    }
    if (guard == 1000000)
        return -1;
    memcpy(tx_buf[i], frame, len);
    tx_ring[i].addr = (u64)(uintptr_t)tx_buf[i];
    tx_ring[i].len = (u16)len;
    tx_ring[i].cmd = 0x0B;               /* EOP | IFCS | RS */
    tx_ring[i].status = 0;
    tx_ring[i].cso = 0;
    tx_ring[i].css = 0;
    tx_ring[i].special = 0;

    tx_index = (i + 1) % TX_COUNT;
    reg_write(REG_TDT, (u32)tx_index);

    for (int guard = 0; guard < 1000000; guard++) {
        if (tx_ring[i].status & 0x01u)
            return 0;
    }
    return -1;
}

void e1000_poll(void)
{
    if (!present)
        return;

    for (int guard = 0; guard < RX_COUNT; guard++) {
        E1000Desc *d = &rx_ring[rx_index];
        u16 status = d->status;
        if (!(status & 0x01u))
            break;
        u32 len = d->len;
        if ((status & 0x02u) && len >= 14 && len <= RX_SIZE)
            netif_input(&eth, rx_buf[rx_index], len);
        d->status = 0;
        reg_write(REG_RDT, (u32)rx_index);
        rx_index = (rx_index + 1) % RX_COUNT;
    }
}

int e1000_present(void) { return present; }

int e1000_init(void)
{
    int bus, dev, func;
    present = 0;
    if (!pci_find(0x8086, 0xFFFF, 0x02, &bus, &dev, &func))
        return -1;
    u16 devid = (u16)(pci_read32(bus, dev, func, PCI_DEVICE) >> 16);
    if (devid != 0x100E && devid != 0x100F && devid != 0x1004 &&
        devid != 0x10D3 && devid != 0x153A)
        return -1;

    u32 bar = pci_read32(bus, dev, func, PCI_BAR0);
    if (bar & 0x1u)                    /* I/O mapped, unsupported */
        return -1;
    u32 mmio_base = bar & 0xFFFFFFF0u;
    if (!mmio_base)
        return -1;

    u16 cmd = pci_read16(bus, dev, func, PCI_COMMAND);
    pci_write16(bus, dev, func, PCI_COMMAND, (u16)(cmd | 0x0006u));

    mmio = (volatile u32 *)(uintptr_t)mmio_base;

    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    timer_delay_ms(10);
    for (int i = 0; i < 1000; i++) {
        if (!(reg_read(REG_CTRL) & CTRL_RST))
            break;
        timer_delay_ms(1);
    }
    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    timer_delay_ms(5);

    for (int i = 0; i < 128; i++)
        reg_write(REG_MTA + i * 4, 0);

    u32 ral = reg_read(REG_RAL);
    u32 rah = reg_read(REG_RAH);
    if (rah & 0x80000000u) {
        mac[0] = (u8)ral; mac[1] = (u8)(ral >> 8);
        mac[2] = (u8)(ral >> 16); mac[3] = (u8)(ral >> 24);
        mac[4] = (u8)rah; mac[5] = (u8)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            u16 w = eeprom_read(i);
            mac[i * 2] = (u8)w;
            mac[i * 2 + 1] = (u8)(w >> 8);
        }
        reg_write(REG_RAL, (u32)mac[0] | ((u32)mac[1] << 8) |
                            ((u32)mac[2] << 16) | ((u32)mac[3] << 24));
        reg_write(REG_RAH, (u32)mac[4] | ((u32)mac[5] << 8) | 0x80000000u);
    }

    int any = 0;
    for (int i = 0; i < 6; i++) any |= mac[i];
    if (!any) {
        /* Locally administered fallback so the stack still has an address. */
        mac[0] = 0x02; mac[1] = 0x52; mac[2] = 0x56;
        mac[3] = 0x4F; mac[4] = 0x53; mac[5] = 0x01;
    }

    memset(tx_ring, 0, sizeof(tx_ring));
    memset(rx_ring, 0, sizeof(rx_ring));
    tx_index = rx_index = 0;

    /* All transmit descriptors start out free (DD set). */
    for (int i = 0; i < TX_COUNT; i++)
        tx_ring[i].status = 0x01;

    reg_write(REG_TDBAL, (u32)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, sizeof(tx_ring));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT);

    for (int i = 0; i < RX_COUNT; i++) {
        rx_ring[i].addr = (u64)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (u32)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, sizeof(rx_ring));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, (u32)(RX_COUNT - 1));
    reg_write(REG_RCTL, RCTL_EN | RCTL_SBP | RCTL_BAM | RCTL_SECRC |
                        RCTL_MPE);

    memset(&eth, 0, sizeof(eth));
    memcpy(eth.name, "eth0", 5);
    memcpy(eth.mac, mac, 6);
    eth.send = e1000_send;
    eth.up = 1;
    net_register(&eth);

    present = 1;
    serial_write("e1000: MAC ");
    for (int i = 0; i < 6; i++) {
        serial_hex(mac[i] >> 4);
        serial_hex(mac[i] & 0xF);
        if (i != 5) serial_write(":");
    }
    serial_write(" link ");
    serial_write((reg_read(REG_STATUS) & 2u) ? "up" : "down");
    serial_write("\r\n");
    return 0;
}
