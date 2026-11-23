#include "pci.h"
#include "io.h"

u32 pci_read32(int bus, int dev, int func, int off)
{
    u32 address = 0x80000000u | ((u32)(bus & 0xFF) << 16) |
                  ((u32)(dev & 0x1F) << 11) | ((u32)(func & 0x07) << 8) |
                  (u32)(off & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_write32(int bus, int dev, int func, int off, u32 value)
{
    u32 address = 0x80000000u | ((u32)(bus & 0xFF) << 16) |
                  ((u32)(dev & 0x1F) << 11) | ((u32)(func & 0x07) << 8) |
                  (u32)(off & 0xFC);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

u16 pci_read16(int bus, int dev, int func, int off)
{
    return (u16)(pci_read32(bus, dev, func, off & 0xFC) >> ((off & 2) * 8));
}

void pci_write16(int bus, int dev, int func, int off, u16 value)
{
    u32 old = pci_read32(bus, dev, func, off & 0xFC);
    u32 shift = (u32)(off & 2) * 8;
    old &= ~(0xFFFFu << shift);
    old |= (u32)value << shift;
    pci_write32(bus, dev, func, off & 0xFC, old);
}

int pci_find(u16 vendor, u16 device, u8 cls, int *bus, int *dev, int *func)
{
    for (int b = 0; b < 8; b++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                u32 id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                u16 v = (u16)(id & 0xFFFF);
                u16 devid = (u16)(id >> 16);
                u8 class = (u8)(pci_read32(b, d, f, PCI_CLASS) >> 24);
                if (vendor != 0xFFFF && v != vendor) continue;
                if (device != 0xFFFF && devid != device) continue;
                if (cls != 0xFF && class != cls) continue;
                *bus = b;
                *dev = d;
                *func = f;
                return 1;
            }
        }
    }
    return 0;
}

int pci_find_class(u8 base, u8 sub, u8 prog_if,
                   int *bus, int *dev, int *func)
{
    for (int b = 0; b < 8; b++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                u32 id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                u32 cls = pci_read32(b, d, f, PCI_CLASS);
                if (((cls >> 24) & 0xFF) != base)
                    continue;
                if (sub != 0xFF && ((cls >> 16) & 0xFF) != sub)
                    continue;
                if (prog_if != 0xFF && ((cls >> 8) & 0xFF) != prog_if)
                    continue;
                *bus = b;
                *dev = d;
                *func = f;
                return 1;
            }
        }
    }
    return 0;
}
