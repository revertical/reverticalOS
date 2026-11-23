#ifndef RVOS_PCI_H
#define RVOS_PCI_H

#include "types.h"

#define PCI_VENDOR 0
#define PCI_DEVICE 2
#define PCI_COMMAND 4
#define PCI_CLASS 9
#define PCI_BAR0 0x10
#define PCI_BAR1 0x14

u32  pci_read32(int bus, int dev, int func, int off);
void pci_write32(int bus, int dev, int func, int off, u32 value);
u16  pci_read16(int bus, int dev, int func, int off);
void pci_write16(int bus, int dev, int func, int off, u16 value);

/* Find the first device matching an optional vendor/device/class mask.
   Pass 0xFFFF for vendor/device and 0xFF for class to ignore a field. */
int pci_find(u16 vendor, u16 device, u8 cls, int *bus, int *dev, int *func);
int pci_find_class(u8 base, u8 sub, u8 prog_if,
                   int *bus, int *dev, int *func);

#endif
