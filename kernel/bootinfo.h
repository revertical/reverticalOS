#ifndef RVOS_BOOTINFO_H
#define RVOS_BOOTINFO_H

#include "types.h"

/* Shared by the BIOS stage 2, the UEFI loader and the kernel.
   The structure is packed because the BIOS side writes it at fixed offsets. */
typedef struct __attribute__((packed)) BootInfo {
    u32 magic;
    u64 fb_phys;
    u32 width;
    u32 height;
    u32 pitch;          /* bytes per scanline */
    u32 bpp;
    u16 mode;
    u8  red_pos;
    u8  red_size;
    u8  green_pos;
    u8  green_size;
    u8  blue_pos;
    u8  blue_size;
    u32 kernel_size;
    u32 boot_drive;     /* BIOS drive number, or a BOOT_DRIVE_* sentinel */
    u64 acpi_rsdp;      /* 0 when the firmware did not provide one */
    u32 flags;
    u32 reserved;
} BootInfo;

#define BOOTINFO_MAGIC 0x4F425652u   /* 'RVBO' */

#define BOOT_DRIVE_CD    0xFFFFFFFFu  /* live CD, no running disk */
#define BOOT_DRIVE_UEFI  0xFFFFFFFEu  /* UEFI boot, no BIOS drive number */

#define BOOT_FLAG_UEFI   0x00000001u

#endif
