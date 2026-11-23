#!/usr/bin/env python3
"""Build a FAT16 EFI System Partition image for the UEFI boot path.

Layout produced:

    /EFI/BOOT/BOOTX64.EFI    the ReverticalOS UEFI loader
    /KERNEL.BIN              the flat 64-bit kernel

The writer is intentionally small and dependency-free; it emits a single
FAT16 volume with contiguous cluster chains (fine for a read-only ESP).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
TOTAL_SECTORS = 32768          # 16 MiB
RESERVED = 4
NUM_FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
SECTORS_PER_CLUSTER = 4        # 2 KiB clusters -> ~8160 clusters (FAT16)
MEDIA = 0xF8
VOLUME_LABEL = b"REVERTICAL "

ATTR_VOLUME = 0x08
ATTR_DIR = 0x10
ATTR_ARCHIVE = 0x20

DIRENT = struct.Struct("<11sBBBHHHHHHHI")


def fat_size_sectors() -> int:
    """Solve the circular FAT16 size equation."""
    size = 1
    while True:
        data = TOTAL_SECTORS - RESERVED - NUM_FATS * size - ROOT_SECTORS
        clusters = data // SECTORS_PER_CLUSTER
        needed = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if needed <= size:
            return size
        size = needed


FAT_SECTORS = fat_size_sectors()
DATA_START = RESERVED + NUM_FATS * FAT_SECTORS + ROOT_SECTORS
CLUSTER_COUNT = (TOTAL_SECTORS - DATA_START) // SECTORS_PER_CLUSTER


def cluster_offset(cluster: int) -> int:
    if cluster < 2:
        raise ValueError("cluster numbers start at 2")
    return (DATA_START + (cluster - 2) * SECTORS_PER_CLUSTER) * SECTOR


def short_name(name: str) -> bytes:
    """Encode an 8.3 name; this ESP only uses names that fit."""
    if "." in name:
        base, ext = name.split(".", 1)
    else:
        base, ext = name, ""
    if len(base) > 8 or len(ext) > 3:
        raise ValueError(f"{name!r} does not fit in 8.3")
    return base.upper().ljust(8).encode("ascii") + \
           ext.upper().ljust(3).encode("ascii")


def dirent(name: bytes, attr: int, cluster: int, size: int) -> bytes:
    return DIRENT.pack(name, attr, 0, 0, 0, 0, 0, 0, 0, 0, cluster, size)


class Builder:
    def __init__(self):
        self.image = bytearray(TOTAL_SECTORS * SECTOR)
        self.fat = [0] * CLUSTER_COUNT
        self.next_cluster = 2
        self.root = bytearray()

        self.fat[0] = 0xFFF8
        if CLUSTER_COUNT > 1:
            self.fat[1] = 0xFFFF

        self.root += dirent(VOLUME_LABEL, ATTR_VOLUME, 0, 0)

    def alloc_chain(self, cluster_count: int) -> int:
        if cluster_count == 0:
            return 0
        first = self.next_cluster
        for i in range(cluster_count):
            c = self.next_cluster
            self.fat[c] = c + 1 if i + 1 < cluster_count else 0xFFFF
            self.next_cluster += 1
        return first

    def write_chain(self, first: int, data: bytes) -> None:
        cluster_bytes = SECTORS_PER_CLUSTER * SECTOR
        c = first
        off = 0
        while off < len(data):
            pos = cluster_offset(c)
            chunk = data[off:off + cluster_bytes]
            self.image[pos:pos + len(chunk)] = chunk
            off += cluster_bytes
            c = self.fat[c]

    def add_file(self, parent: bytearray, name: str, data: bytes) -> None:
        clusters = (len(data) + SECTORS_PER_CLUSTER * SECTOR - 1) // \
                   (SECTORS_PER_CLUSTER * SECTOR)
        first = self.alloc_chain(clusters)
        self.write_chain(first, data)
        parent.extend(dirent(short_name(name), ATTR_ARCHIVE, first, len(data)))

    def add_dir(self, parent: bytearray, name: str) -> int:
        """Allocate one cluster for a directory and return its cluster."""
        first = self.alloc_chain(1)
        self.image[cluster_offset(first):cluster_offset(first) + 64] = b"\0" * 64
        parent.extend(dirent(short_name(name), ATTR_DIR, first, 0))
        return first

    def dir_entries(self, cluster: int, parent_cluster: int,
                    entries: bytes) -> None:
        cluster_bytes = SECTORS_PER_CLUSTER * SECTOR
        blob = bytearray(cluster_bytes)
        dot = dirent(b".          ", ATTR_DIR, cluster, 0)
        dotdot = dirent(b"..         ", ATTR_DIR, parent_cluster, 0)
        blob[0:32] = dot
        blob[32:64] = dotdot
        blob[64:64 + len(entries)] = entries
        pos = cluster_offset(cluster)
        self.image[pos:pos + cluster_bytes] = blob

    def finish_boot_sector(self) -> None:
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"REVERTOS"
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", bs, 14, RESERVED)
        bs[16] = NUM_FATS
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, TOTAL_SECTORS & 0xFFFF)
        bs[21] = MEDIA
        struct.pack_into("<H", bs, 22, FAT_SECTORS)
        struct.pack_into("<H", bs, 24, 32)      # sectors per track
        struct.pack_into("<H", bs, 26, 64)      # heads
        struct.pack_into("<I", bs, 28, 0)       # hidden sectors
        # FAT12/16 volumes use the 16-bit count; the 32-bit field must be 0.
        struct.pack_into("<I", bs, 32, 0)
        bs[36] = 0x80                            # drive number
        bs[38] = 0x29                            # extended boot signature
        struct.pack_into("<I", bs, 39, 0x52564F53)
        bs[43:54] = VOLUME_LABEL
        bs[54:62] = b"FAT16   "
        bs[62:66] = b"\xFA\xF4\xEB\xFC"          # cli; hlt; jmp $-4
        bs[510:512] = b"\x55\xAA"
        self.image[0:SECTOR] = bs

        fat = bytearray(FAT_SECTORS * SECTOR)
        for i, value in enumerate(self.fat):
            struct.pack_into("<H", fat, i * 2, value)
        for n in range(NUM_FATS):
            pos = (RESERVED + n * FAT_SECTORS) * SECTOR
            self.image[pos:pos + len(fat)] = fat

        root_pos = (RESERVED + NUM_FATS * FAT_SECTORS) * SECTOR
        self.image[root_pos:root_pos + len(self.root)] = self.root


def build(loader: bytes, kernel: bytes) -> bytes:
    b = Builder()

    efi_cluster = b.add_dir(b.root, "EFI")
    boot_cluster = b.alloc_chain(1)
    b.add_file(b.root, "KERNEL.BIN", kernel)

    efi_entries = bytearray()
    efi_entries += dirent(short_name("BOOT"), ATTR_DIR, boot_cluster, 0)
    b.dir_entries(efi_cluster, 0, efi_entries)

    boot_entries = bytearray()
    b.add_file(boot_entries, "BOOTX64.EFI", loader)
    b.dir_entries(boot_cluster, efi_cluster, boot_entries)

    b.finish_boot_sector()
    return bytes(b.image)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--loader", default="build/bootx64.efi")
    ap.add_argument("--kernel", default="build/kernel.bin")
    ap.add_argument("--output", default="build/esp.img")
    args = ap.parse_args()

    loader = Path(args.loader).read_bytes()
    kernel = Path(args.kernel).read_bytes()
    if len(loader) >= 8 * 1024 * 1024:
        print("loader is implausibly large", file=sys.stderr)
        return 1

    image = build(loader, kernel)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"esp: {out} ({len(image)} bytes, FAT16, "
          f"{CLUSTER_COUNT} clusters)")
    print(f"  /EFI/BOOT/BOOTX64.EFI ({len(loader)} bytes)")
    print(f"  /KERNEL.BIN ({len(kernel)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
