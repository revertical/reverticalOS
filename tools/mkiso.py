#!/usr/bin/env python3
"""Build the bootable ReverticalOS live CD (ISO 9660 + El Torito + hybrid MBR).

The ISO uses our own two-stage loader, not GRUB or Limine:

    LBA 0        hybrid MBR: one 0xEF partition covering the ESP, plus a stub
                 that chainloads the El Torito BIOS stage 1 for USB boot
    LBA 1..15    ISO 9660 system area (zero)
    LBA 16       Primary Volume Descriptor
    LBA 17       El Torito Boot Record Volume Descriptor
    LBA 18       Volume Descriptor Set Terminator
    LBA 19       El Torito Boot Catalog
    LBA 20       cdstage1.bin (2048-byte no-emulation boot image)
    LBA 21..24   stage2-cd.bin (8 KiB, loaded by cdstage1)
    LBA 25       kernel header (RVK1, padded to 2048 bytes)
    LBA 26..     kernel flat binary (padded to 2048-byte blocks)
    ...          root / EFI / EFI/BOOT directories
    ...          L and M path tables
    ...          FAT16 ESP (the UEFI El Torito image)
    ...          /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN file data

The volume descriptor set is contiguous (16, 17, 18) as ISO 9660 requires;
the boot catalog lives after the terminator, exactly where xorriso puts it.

The ISO carries a real ISO 9660 directory tree in addition to the El Torito
entries.  Firmware that boots the UEFI El Torito image uses the FAT16 ESP;
tools and firmware that instead scan the ISO 9660 tree for the removable
media path find /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN there.

The same file can be written to a USB stick with dd.  Real firmware (Dell,
ThinkPad, ...) does not use an El Torito entry on a USB mass-storage device;
it looks for a partition containing \\EFI\\BOOT\\BOOTX64.EFI.  The hybrid MBR
in the ISO system area describes the ESP as a normal 0xEF partition, so the
UEFI boot entry appears, and legacy BIOS boots the MBR stub into the same
stage 1 the CD uses.

The CD stage 2 uses 2048-byte blocks for INT 13h reads and looks for the
kernel at LBA 25/26.  Its BootInfo.boot_drive is 0xFFFFFFFF, which tells the
kernel and installer that no hard disk is the running system.
"""
import argparse
import struct
import sys
from pathlib import Path

BLOCK = 2048
KERNEL_MAGIC = 0x314B5652       # "RVK1"
LOAD_ADDR = 0x100000
KERNEL_MAX = 1536 * 1024

PVD_LBA = 16
BOOT_RECORD_LBA = 17
TERMINATOR_LBA = 18
BOOT_CATALOG_LBA = 19
CDSTAGE1_LBA = 20
CDSTAGE2_LBA = 21
CDSTAGE2_BLOCKS = 4             # 8192 bytes
KERNEL_HDR_LBA = 25
KERNEL_DATA_LBA = 26
MIN_BLOCKS = 512                # keep the image at least 1 MiB
ESP_PART_TYPE = 0xEF            # EFI System Partition
MBR_DISK_ID = 0x534F5652        # "RVOS"

# Recording date used in directory records: 2026-11-18 00:00:00 +00:00
DIR_DATE = bytes([126, 11, 18, 0, 0, 0, 0])


def both16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def text_field(text: str, size: int, pad: bytes = b" ") -> bytes:
    raw = text.encode("ascii")
    if len(raw) > size:
        raise ValueError(f"{text!r} does not fit in {size} bytes")
    return raw + pad * (size - len(raw))


def dir_record(name: bytes, extent: int, size: int, flags: int) -> bytes:
    length = 33 + len(name)
    if len(name) % 2 == 0:
        length += 1             # padding when the identifier is even
    rec = bytearray(length)
    rec[0] = length
    rec[1] = 0                  # extended attribute length
    rec[2:10] = both32(extent)
    rec[10:18] = both32(size)
    rec[18:25] = DIR_DATE
    rec[25] = flags
    rec[26] = 0                 # file unit size
    rec[27] = 0                 # interleave gap
    rec[28:32] = both16(1)      # volume sequence number
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def path_table_entry(name: bytes, extent: int, parent: int,
                     big_endian: bool) -> bytes:
    entry = bytearray(8 + len(name))
    entry[0] = len(name)
    entry[1] = 0                # extended attribute length
    if big_endian:
        struct.pack_into(">I", entry, 2, extent)
        struct.pack_into(">H", entry, 6, parent)
    else:
        struct.pack_into("<I", entry, 2, extent)
        struct.pack_into("<H", entry, 6, parent)
    entry[8:8 + len(name)] = name
    if len(name) % 2 == 1:
        entry += b"\x00"        # pad to an even length
    return bytes(entry)


def make_path_table(dirs, big_endian: bool) -> bytes:
    """dirs is an ordered list of (name, extent, parent_number)."""
    out = bytearray()
    for name, extent, parent in dirs:
        out += path_table_entry(name, extent, parent, big_endian)
    return bytes(out)


def make_pvd(total_blocks: int, root_lba: int, root_size: int,
             path_size: int, l_path_lba: int, m_path_lba: int) -> bytes:
    pvd = bytearray(BLOCK)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = text_field("REVERTICALOS", 32)
    pvd[40:72] = text_field("REVERTICALOS", 32)
    # 72..79 reserved, 88..119 escape sequences
    pvd[80:88] = both32(total_blocks)
    pvd[120:124] = both16(1)            # volume set size
    pvd[124:128] = both16(1)            # volume sequence number
    pvd[128:132] = both16(BLOCK)        # logical block size
    pvd[132:140] = both32(path_size)    # path table size
    pvd[140:144] = struct.pack("<I", l_path_lba)
    pvd[144:148] = struct.pack("<I", 0)
    pvd[148:152] = struct.pack(">I", m_path_lba)
    pvd[152:156] = struct.pack(">I", 0)

    root_record = dir_record(b"\x00", root_lba, root_size, 0x02)
    pvd[156:156 + len(root_record)] = root_record

    pvd[190:318] = text_field("REVERTICALOS", 128)
    pvd[318:446] = text_field("REVERTICALOS", 128)
    pvd[446:574] = text_field("ReverticalOS installer", 128)
    pvd[574:702] = text_field("REVERTICALOS LIVE CD", 128)
    date = b"2026111800000000\x00"
    pvd[813:830] = date
    pvd[830:847] = date
    pvd[847:864] = date
    pvd[864:881] = date
    pvd[881] = 1                        # file structure version
    return bytes(pvd)


def chs(lba: int) -> bytes:
    """CHS address for an MBR partition entry (255 heads, 63 sectors)."""
    heads, spt = 255, 63
    cyl = lba // (heads * spt)
    rem = lba % (heads * spt)
    if cyl > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    head = rem // spt
    sector = rem % spt + 1
    return bytes([head & 0xFF,
                  (sector & 0x3F) | ((cyl >> 2) & 0xC0),
                  cyl & 0xFF])


def make_hybrid_mbr(code: bytes, start_512: int, sectors_512: int) -> bytes:
    """MBR with one EFI System Partition entry covering the FAT16 ESP."""
    if len(code) > 440:
        raise ValueError("MBR boot code must be at most 440 bytes")
    mbr = bytearray(512)
    mbr[0:len(code)] = code
    struct.pack_into("<I", mbr, 440, MBR_DISK_ID)
    entry = bytearray(16)
    entry[0] = 0x80                             # active
    entry[1:4] = chs(start_512)
    entry[4] = ESP_PART_TYPE
    entry[5:8] = chs(start_512 + sectors_512 - 1)
    struct.pack_into("<I", entry, 8, start_512)
    struct.pack_into("<I", entry, 12, sectors_512)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


def make_boot_record() -> bytes:
    rec = bytearray(BLOCK)
    rec[0] = 0
    rec[1:6] = b"CD001"
    rec[6] = 1
    rec[7:39] = text_field("EL TORITO SPECIFICATION", 32, b"\x00")
    struct.pack_into("<I", rec, 71, BOOT_CATALOG_LBA)
    return bytes(rec)


def make_boot_catalog(esp_lba: int, esp_sectors: int) -> bytes:
    catalog = bytearray(BLOCK)

    validation = bytearray(32)
    validation[0] = 1                   # header id
    validation[1] = 0                   # 80x86 platform
    validation[30:32] = b"\x55\xAA"
    checksum = (-sum(struct.unpack("<16H", validation))) & 0xFFFF
    struct.pack_into("<H", validation, 28, checksum)
    catalog[0:32] = validation

    initial = bytearray(32)
    initial[0] = 0x88                   # bootable
    initial[1] = 0                      # no emulation
    struct.pack_into("<H", initial, 2, 0)       # load segment 0:7C00
    initial[4] = 0                       # system type (ignored here)
    struct.pack_into("<H", initial, 6, 4)       # 4 x 512-byte sectors
    struct.pack_into("<I", initial, 8, CDSTAGE1_LBA)
    catalog[32:64] = initial

    if esp_lba:
        # Section header: EFI platform, one entry, final section.
        header = bytearray(32)
        header[0] = 0x91                 # final section header
        header[1] = 0xEF                 # EFI system partition
        struct.pack_into("<H", header, 2, 1)
        catalog[64:96] = header

        # No-emulation entry pointing at the FAT16 ESP image.
        entry = bytearray(32)
        entry[0] = 0x88
        entry[1] = 0
        struct.pack_into("<H", entry, 2, 0)
        entry[4] = 0
        struct.pack_into("<H", entry, 6, min(esp_sectors, 0xFFFF))
        struct.pack_into("<I", entry, 8, esp_lba)
        catalog[96:128] = entry

    return bytes(catalog)


def name_bytes(name: str) -> bytes:
    raw = name.encode("ascii")
    if len(raw) > 30:
        raise ValueError(f"ISO 9660 name too long: {name!r}")
    return raw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cdstage1", default="build/cdstage1.bin")
    ap.add_argument("--stage2", default="build/stage2-cd.bin")
    ap.add_argument("--kernel", default="build/kernel.bin")
    ap.add_argument("--esp", default="build/esp.img",
                    help="FAT16 EFI System Partition image for UEFI boot")
    ap.add_argument("--loader", default="build/bootx64.efi",
                    help="UEFI loader exposed as /EFI/BOOT/BOOTX64.EFI")
    ap.add_argument("--mbr", default="build/mbr.bin",
                    help="440-byte hybrid MBR stub from boot/mbr.asm")
    ap.add_argument("--output", default="build/revertical.iso")
    args = ap.parse_args()

    cdstage1 = Path(args.cdstage1).read_bytes()
    stage2 = Path(args.stage2).read_bytes()
    kernel = Path(args.kernel).read_bytes()
    esp = Path(args.esp).read_bytes() if args.esp and Path(args.esp).exists() \
        else b""
    loader = Path(args.loader).read_bytes() \
        if args.loader and Path(args.loader).exists() else b""
    mbr_code = Path(args.mbr).read_bytes() \
        if args.mbr and Path(args.mbr).exists() else b""

    if len(mbr_code) > 440:
        print(f"MBR code must be at most 440 bytes (got {len(mbr_code)})",
              file=sys.stderr)
        return 1
    if esp and not mbr_code:
        print(f"warning: {args.mbr} missing; ISO will not boot from USB",
              file=sys.stderr)

    if len(cdstage1) != BLOCK:
        print(f"cdstage1 must be exactly {BLOCK} bytes (got {len(cdstage1)})",
              file=sys.stderr)
        return 1
    if len(stage2) != CDSTAGE2_BLOCKS * BLOCK:
        print(f"CD stage2 must be exactly {CDSTAGE2_BLOCKS * BLOCK} bytes "
              f"(got {len(stage2)})", file=sys.stderr)
        return 1
    if len(kernel) > KERNEL_MAX:
        print(f"kernel is too large: {len(kernel)} > {KERNEL_MAX}",
              file=sys.stderr)
        return 1

    kernel_blocks = (len(kernel) + BLOCK - 1) // BLOCK
    esp_blocks = (len(esp) + BLOCK - 1) // BLOCK if esp else 0
    loader_blocks = (len(loader) + BLOCK - 1) // BLOCK if loader else 0

    # --- Lay out every extent before writing any directory record. ---------
    pos = KERNEL_DATA_LBA + kernel_blocks

    root_lba = pos
    pos += 1
    efi_lba = pos
    pos += 1
    boot_dir_lba = pos
    pos += 1
    l_path_lba = pos
    pos += 1
    m_path_lba = pos
    pos += 1

    esp_lba = 0
    if esp_blocks:
        esp_lba = ((pos + 3) // 4) * 4      # keep the image 4-block aligned
        # The UEFI entry counts 512-byte sectors.
        esp_sectors = esp_blocks * (BLOCK // 512)
        if esp_sectors > 0xFFFF:
            print("ESP image is too large for an El Torito entry",
                  file=sys.stderr)
            return 1
        pos = esp_lba + esp_blocks
    else:
        esp_sectors = 0

    loader_lba = pos
    pos += loader_blocks
    kernel_file_lba = pos
    pos += kernel_blocks

    total_blocks = max(MIN_BLOCKS, pos)

    # --- ISO 9660 directory tree. -----------------------------------------
    # Root: EFI (directory) and KERNEL.BIN (file).
    root_entries = []
    if loader:
        root_entries.append((name_bytes("EFI"), efi_lba, BLOCK, 0x02))
    root_entries.append((name_bytes("KERNEL.BIN"), kernel_file_lba,
                         len(kernel), 0x00))
    root_entries.sort(key=lambda e: e[0])

    efi_entries = []
    if loader:
        efi_entries.append((name_bytes("BOOT"), boot_dir_lba, BLOCK, 0x02))
    efi_entries.sort(key=lambda e: e[0])

    boot_entries = []
    if loader:
        boot_entries.append((name_bytes("BOOTX64.EFI"), loader_lba,
                             len(loader), 0x00))
    boot_entries.sort(key=lambda e: e[0])

    def build_dir(entries, own_lba: int, parent_lba: int) -> bytes:
        data = bytearray(BLOCK)
        off = 0
        for name, extent, size, flags in (
                [(b"\x00", own_lba, BLOCK, 0x02),
                 (b"\x01", parent_lba, BLOCK, 0x02)] + entries):
            rec = dir_record(name, extent, size, flags)
            if off + len(rec) > BLOCK:
                raise SystemExit("directory does not fit in one block")
            data[off:off + len(rec)] = rec
            off += len(rec)
        return bytes(data)

    root_dir = build_dir(root_entries, root_lba, root_lba)
    efi_dir = build_dir(efi_entries, efi_lba, root_lba)
    boot_dir = build_dir(boot_entries, boot_dir_lba, efi_lba)

    # Breadth-first order: root (1), EFI (2), BOOT (3).
    dir_table = [(b"\x00", root_lba, 1)]
    if loader:
        dir_table.append((name_bytes("EFI"), efi_lba, 1))
        dir_table.append((name_bytes("BOOT"), boot_dir_lba, 2))
    l_table = make_path_table(dir_table, big_endian=False)
    m_table = make_path_table(dir_table, big_endian=True)

    # --- Write the image. --------------------------------------------------
    image = bytearray(total_blocks * BLOCK)

    def put(lba: int, data: bytes) -> None:
        end = lba * BLOCK + len(data)
        if end > len(image):
            raise SystemExit(f"data at LBA {lba} does not fit in the ISO")
        image[lba * BLOCK:end] = data

    put(PVD_LBA, make_pvd(total_blocks, root_lba, BLOCK,
                          len(l_table), l_path_lba, m_path_lba))
    put(BOOT_RECORD_LBA, make_boot_record())
    put(TERMINATOR_LBA, bytes([255]) + b"CD001" + bytes([1]))
    put(BOOT_CATALOG_LBA, make_boot_catalog(esp_lba, esp_sectors))
    put(CDSTAGE1_LBA, cdstage1)
    put(CDSTAGE2_LBA, stage2)

    header = struct.pack("<IIIII", KERNEL_MAGIC, len(kernel),
                         LOAD_ADDR, LOAD_ADDR, 0)
    put(KERNEL_HDR_LBA, header.ljust(BLOCK, b"\x00"))
    put(KERNEL_DATA_LBA, kernel)

    put(root_lba, root_dir)
    put(efi_lba, efi_dir)
    put(boot_dir_lba, boot_dir)
    put(l_path_lba, l_table)
    put(m_path_lba, m_table)
    if esp_blocks:
        esp_in_iso = bytearray(esp)
        if len(esp_in_iso) >= 32:
            # The ESP is a partition of the USB image, not a bare volume.
            struct.pack_into("<I", esp_in_iso, 28,
                             esp_lba * (BLOCK // 512))
        put(esp_lba, bytes(esp_in_iso))
    if loader_blocks:
        put(loader_lba, loader)
    put(kernel_file_lba, kernel)

    # Hybrid MBR in the ISO 9660 system area: the ESP becomes a normal 0xEF
    # partition, so UEFI firmware on a USB stick offers a boot entry.
    if esp_blocks and mbr_code:
        start_512 = esp_lba * (BLOCK // 512)
        if start_512 + esp_sectors > total_blocks * (BLOCK // 512):
            print("ESP partition does not fit in the ISO", file=sys.stderr)
            return 1
        put(0, make_hybrid_mbr(mbr_code, start_512, esp_sectors))

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)

    print(f"iso: {out} ({len(image)} bytes, {total_blocks} blocks)")
    print(f"  stage1 LBA {CDSTAGE1_LBA}  stage2 LBA {CDSTAGE2_LBA}"
          f"..{CDSTAGE2_LBA + CDSTAGE2_BLOCKS - 1}")
    print(f"  kernel header LBA {KERNEL_HDR_LBA}  "
          f"kernel LBA {KERNEL_DATA_LBA}..{KERNEL_DATA_LBA + kernel_blocks - 1}"
          f" ({len(kernel)} bytes)")
    print(f"  root dir LBA {root_lba}  EFI LBA {efi_lba}  "
          f"BOOT LBA {boot_dir_lba}")
    print(f"  path tables {l_path_lba}/{m_path_lba} "
          f"({len(l_table)} bytes each)")
    if esp_blocks:
        print(f"  UEFI ESP LBA {esp_lba}..{esp_lba + esp_blocks - 1} "
              f"({len(esp)} bytes)")
        if mbr_code:
            print(f"  hybrid MBR: partition 1 type EF at LBA "
                  f"{esp_lba * (BLOCK // 512)}, {esp_sectors} x 512B sectors")
    else:
        print("  UEFI ESP not included (BIOS-only ISO)")
    if loader_blocks:
        print(f"  /EFI/BOOT/BOOTX64.EFI LBA {loader_lba} ({len(loader)} bytes)")
        print(f"  /KERNEL.BIN LBA {kernel_file_lba} ({len(kernel)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
