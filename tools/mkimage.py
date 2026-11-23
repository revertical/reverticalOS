#!/usr/bin/env python3
"""Assemble the bootable ReverticalOS raw disk image.

Layout:
    LBA 0            stage 1 boot sector (MBR + partition table)
    LBA 1..16        stage 2 bootloader (8 KiB)
    LBA 17           kernel header
    LBA 18..         flat kernel binary
    LBA 2048..       RVFS volume (partition type 0x52)

The kernel finds the volume by scanning the MBR, so the same layout is what the
GUI installer writes to a target disk.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvfs  # noqa: E402

KERNEL_MAGIC = 0x314B5652  # "RVK1"
LOAD_ADDR = 0x100000
KERNEL_MAX = 1536 * 1024
PART_START = 2048          # 1 MiB
PART_TYPE = 0x52           # RVFS


WELCOME = """ReverticalOS
============

This volume is a real, writable RVFS filesystem living on an ATA disk.

It was created by tools/rvfs.py on the build machine, and is mounted by
kernel/rvfs.c at boot. The Files app reads and writes through the same
driver the installer uses.

Nothing here is a Linux filesystem: no ext4, no FAT. Blocks, inodes,
bitmaps and directory entries are our own format.
"""

README = """ReverticalOS - a desktop rebuilt from scratch.

Everything you see is written from zero: the bootloader, the kernel, the
compositor, the window manager, the boot animation and this filesystem.

Try the Files app: navigate folders, select a text file and read it in the
preview pane. The Installer app can write this system onto another disk.

Version 2026.11
"""

NOTES = """Roadmap
-------
* Revertical AI model link (awaiting system prompt)
* Keyboard driver and text entry
* More file operations: rename, copy, move
* User accounts and permissions
"""

VERSION = """ReverticalOS 2026.11
kernel: 2026.11
filesystem: RVFS 1.0
compositor: software, 32bpp linear framebuffer
"""

HELLO = """Hello, Revertical.

This file lives in /Users/revertical on the RVFS volume. The installer
creates the same tree on a freshly installed disk.
"""

MODULES = """ReverticalOS modules
===================

.rvx  RVEX executable: image, entry point, stack size, named imports
.lib  RVLB library: image and named exports

Double-click a .rvx in Files to load it. The kernel resolves imports against
its own API and against every .lib in /System/Libs, rebases the image,
switches to a private stack and calls _start().
"""


def sample_files(apps_dir: str) -> dict[str, bytes]:
    files = {
        "/Readme.txt": README.encode(),
        "/Documents/welcome.txt": WELCOME.encode(),
        "/Documents/notes.txt": NOTES.encode(),
        "/System/version.txt": VERSION.encode(),
        "/System/Apps/README.txt": MODULES.encode(),
        "/Users/revertical/hello.txt": HELLO.encode(),
        "/Pictures/README.txt": b"Drop QOI images here in a later build.\n",
    }
    if apps_dir:
        root = Path(apps_dir)
        for path in sorted(root.glob("*.rvx")):
            files["/System/Apps/" + path.name] = path.read_bytes()
        for path in sorted(root.glob("*.lib")):
            files["/System/Libs/" + path.name] = path.read_bytes()
    return files


def mbr_partition(start: int, sectors: int, ptype: int) -> bytes:
    entry = bytearray(16)
    entry[0] = 0x80                 # bootable
    entry[1:4] = b"\xFE\xFF\xFF"    # CHS start: use LBA
    entry[4] = ptype
    entry[5:8] = b"\xFE\xFF\xFF"    # CHS end: use LBA
    struct.pack_into("<II", entry, 8, start, sectors)
    return bytes(entry)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage1", default="build/stage1.bin")
    ap.add_argument("--stage2", default="build/stage2.bin")
    ap.add_argument("--kernel", default="build/kernel.bin")
    ap.add_argument("--output", default="build/revertical.img")
    ap.add_argument("--size", default="32M")
    ap.add_argument("--apps-dir", default="build/apps")
    ap.add_argument("--no-fs", action="store_true")
    args = ap.parse_args()

    stage1 = bytearray(Path(args.stage1).read_bytes())
    stage2 = Path(args.stage2).read_bytes()
    kernel = Path(args.kernel).read_bytes()

    if len(stage1) != 512:
        print(f"stage1 must be exactly 512 bytes (got {len(stage1)})", file=sys.stderr)
        return 1
    if len(stage2) != 16 * 512:
        print(f"stage2 must be exactly 8192 bytes (got {len(stage2)})", file=sys.stderr)
        return 1
    if len(kernel) > KERNEL_MAX:
        print(f"kernel is too large: {len(kernel)} > {KERNEL_MAX}", file=sys.stderr)
        return 1

    header = struct.pack("<IIIII", KERNEL_MAGIC, len(kernel), LOAD_ADDR, LOAD_ADDR, 0)
    header = header.ljust(512, b"\0")

    if args.size.endswith("M"):
        total = int(args.size[:-1]) * 1024 * 1024
    elif args.size.endswith("K"):
        total = int(args.size[:-1]) * 1024
    else:
        total = int(args.size)

    prefix = stage1 + stage2 + header + kernel
    part_sectors = max(0, total // 512 - PART_START)
    if total < len(prefix) + PART_START * 512:
        total = (len(prefix) + PART_START * 512 + 1024 * 1024 - 1) // (1024 * 1024)
        total *= 1024 * 1024
        part_sectors = total // 512 - PART_START
    if total < len(prefix):
        total = (len(prefix) + 1023) // 1024 * 1024 + 1024 * 1024
        part_sectors = max(0, total // 512 - PART_START)

    # MBR partition table (the boot code ignores it, the kernel does not).
    stage1[446:462] = mbr_partition(PART_START, part_sectors, PART_TYPE)
    stage1[462:510] = b"\0" * 48
    stage1[510:512] = b"\x55\xAA"

    image = bytearray(total)
    image[0:len(prefix)] = prefix
    image[446:510] = stage1[446:510]

    if part_sectors and not args.no_fs:
        volume = rvfs.create_volume(part_sectors, "ReverticalOS",
                                    sample_files(args.apps_dir))
        off = PART_START * 512
        if len(volume) > len(image) - off:
            print("filesystem does not fit in the image", file=sys.stderr)
            return 1
        image[off:off + len(volume)] = volume
        print(f"rvfs: {len(volume)} bytes at LBA {PART_START}")

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"image: {out} ({len(image)} bytes, kernel {len(kernel)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
