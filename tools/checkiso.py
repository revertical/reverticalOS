#!/usr/bin/env python3
"""Static validator for build/revertical.iso.

Checks the things a UEFI/BIOS machine looks at before it can boot:

  * the ISO 9660 volume descriptor set is contiguous and terminated,
  * the El Torito catalog is valid and has a bootable EFI section entry,
  * the UEFI El Torito image is a sane FAT16 volume,
  * /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN are readable through FAT,
  * the same two files exist in the ISO 9660 tree with identical contents,
  * the hybrid MBR describes the ESP, so USB firmware offers a boot entry.

It does not boot anything; run it after `make iso` to catch builder bugs.
"""
import struct
import sys
from pathlib import Path

BLOCK = 2048


class Check:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.passes = 0

    def ok(self, text: str) -> None:
        self.passes += 1
        print(f"  ok   {text}")

    def bad(self, text: str) -> None:
        self.failures.append(text)
        print(f"  FAIL {text}")

    def expect(self, cond: bool, text: str) -> bool:
        if cond:
            self.ok(text)
        else:
            self.bad(text)
        return cond


def u16(b: bytes, off: int) -> int:
    return struct.unpack_from("<H", b, off)[0]


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def parse_entries(blob: bytes, size: int) -> list[tuple[bytes, int, int, int]]:
    out: list[tuple[bytes, int, int, int]] = []
    off = 0
    while off + 32 <= min(len(blob), size):
        if blob[off] == 0:
            break
        if blob[off] == 0xE5:
            off += 32
            continue
        name = bytes(blob[off:off + 11])
        attr = blob[off + 11]
        if attr == 0x08:            # volume label
            off += 32
            continue
        cl = u16(blob, off + 26)
        sz = u32(blob, off + 28)
        out.append((name, attr, cl, sz))
        off += 32
    return out


def parse_dir(img: bytes, fat: list[int], data_start: int, spc: int,
              cluster: int, size: int) -> list[tuple[bytes, int, int, int]]:
    blob = bytearray()
    seen = set()
    while 2 <= cluster < 0xFFF8 and cluster not in seen:
        seen.add(cluster)
        off = (data_start + (cluster - 2) * spc) * 512
        blob += img[off:off + spc * 512]
        cluster = fat[cluster]
    return parse_entries(bytes(blob), size if size else len(blob))


def read_fat_file(img: bytes, fat: list[int], data_start: int, spc: int,
                  cluster: int, size: int) -> bytes:
    data = bytearray()
    seen = set()
    while 2 <= cluster < 0xFFF8 and cluster not in seen:
        seen.add(cluster)
        off = (data_start + (cluster - 2) * spc) * 512
        data += img[off:off + spc * 512]
        cluster = fat[cluster]
    return bytes(data[:size])


def fat_name(raw: bytes) -> str:
    base = raw[:8].decode("ascii", "replace").rstrip()
    ext = raw[8:11].decode("ascii", "replace").rstrip()
    return (base + "." + ext) if ext else base


def walk_fat(img: bytes, path: str) -> bytes | None:
    bps = u16(img, 11)
    spc = img[13]
    res = u16(img, 14)
    nfats = img[16]
    rootent = u16(img, 17)
    tot16 = u16(img, 19)
    fatsz = u16(img, 22)
    tot32 = u32(img, 32)
    total = tot16 or tot32
    root_secs = (rootent * 32 + bps - 1) // bps
    data_start = res + nfats * fatsz + root_secs
    fat_off = res * bps
    fat = [u16(img, fat_off + i * 2) for i in range(fatsz * bps // 2)]

    parts = [p for p in path.split("/") if p]
    root_off = (res + nfats * fatsz) * bps
    entries = parse_entries(img[root_off:root_off + rootent * 32],
                            rootent * 32)
    cluster = 0
    size = 0
    attr = 0
    for part in parts:
        found = None
        for name, a, cl, sz in entries:
            if fat_name(name).upper() == part.upper():
                found = (a, cl, sz)
                break
        if not found:
            return None
        attr, cluster, size = found
        if attr & 0x10:
            entries = parse_dir(img, fat, data_start, spc, cluster, size)
        elif part != parts[-1]:
            return None
    if attr & 0x10 or cluster < 2:
        return None
    return read_fat_file(img, fat, data_start, spc, cluster, size)


def iso_dir(img: bytes, extent: int, size: int):
    blob = img[extent * BLOCK:extent * BLOCK + size]
    off = 0
    while off < size:
        ln = blob[off]
        if ln == 0:
            break
        rec = blob[off:off + ln]
        nl = rec[32]
        yield (rec[33:33 + nl], u32(rec, 2), u32(rec, 10), rec[25])
        off += ln


def walk_iso(img: bytes, path: str) -> bytes | None:
    pvd = img[16 * BLOCK:17 * BLOCK]
    root = pvd[156:156 + pvd[156]]
    extent, size = u32(root, 2), u32(root, 10)
    for part in [p for p in path.split("/") if p]:
        found = None
        for name, ext, sz, flags in iso_dir(img, extent, size):
            if name in (b"\x00", b"\x01"):
                continue
            base = name.split(b";")[0].decode("ascii", "replace")
            if base.upper() == part.upper():
                found = (ext, sz, flags)
                break
        if not found:
            return None
        extent, size, flags = found
    return img[extent * BLOCK:extent * BLOCK + size]


def main() -> int:
    iso_path = Path(sys.argv[1] if len(sys.argv) > 1
                    else "build/revertical.iso")
    if not iso_path.exists():
        print(f"{iso_path}: not found (run make iso)")
        return 1
    img = iso_path.read_bytes()
    c = Check()

    print(f"ISO: {iso_path} ({len(img)} bytes, {len(img) // BLOCK} blocks)")
    c.expect(len(img) % BLOCK == 0, "size is a multiple of 2048")

    print("volume descriptors")
    for lba, vd_type in ((16, 1), (17, 0), (18, 0xFF)):
        d = img[lba * BLOCK:(lba + 1) * BLOCK]
        c.expect(d[0] == vd_type and d[1:6] == b"CD001",
                 f"LBA {lba} is descriptor type {vd_type:#x}")

    print("El Torito catalog")
    br = img[17 * BLOCK:18 * BLOCK]
    c.expect(br[7:39].rstrip(b"\x00") == b"EL TORITO SPECIFICATION",
             "boot record system id")
    cat_lba = u32(br, 71)
    c.expect(cat_lba != 0 and cat_lba < len(img) // BLOCK,
             f"catalog LBA {cat_lba}")
    cat = img[cat_lba * BLOCK:(cat_lba + 1) * BLOCK]
    c.expect(cat[0] == 1, "validation entry header id")
    c.expect(sum(struct.unpack_from("<16H", cat, 0)) & 0xFFFF == 0,
             "validation checksum sums to zero")
    c.expect(cat[30:32] == b"\x55\xAA", "validation key 55 AA")

    efi_lba = efi_sectors = 0
    bios = None
    in_efi = False
    for i in range(1, 64):
        e = cat[i * 32:(i + 1) * 32]
        if e[0] in (0x90, 0x91):
            in_efi = e[1] == 0xEF
            continue
        if e[0] != 0x88:
            continue
        lba, count = u32(e, 8), u16(e, 6)
        if in_efi:
            efi_lba, efi_sectors = lba, count
        elif bios is None:
            bios = (lba, count)
    c.expect(bios is not None and bios[0] != 0,
             f"BIOS entry at LBA {bios[0] if bios else '?'}")
    c.expect(efi_lba != 0 and efi_sectors != 0,
             f"UEFI entry at LBA {efi_lba}, {efi_sectors} sectors")
    if not (efi_lba and efi_sectors):
        return 1

    esp = img[efi_lba * BLOCK:efi_lba * BLOCK + efi_sectors * 512]

    print("hybrid MBR (USB)")
    mbr = img[0:512]
    c.expect(mbr[510:512] == b"\x55\xAA", "MBR signature 55 AA")
    c.expect(mbr[0] != 0, "MBR boot stub present")
    part = mbr[446:462]
    c.expect(part[4] == 0xEF, "partition 1 type is 0xEF (EFI System)")
    pstart, pcount = u32(part, 8), u32(part, 12)
    c.expect(pstart * 512 == efi_lba * BLOCK,
             f"partition 1 starts at LBA {pstart} "
             f"(ESP is at {efi_lba} x 2048B)")
    c.expect(pcount == efi_sectors,
             f"partition 1 is {pcount} x 512B sectors")
    c.expect((pstart + pcount) * 512 <= len(img),
             "partition 1 fits inside the image")
    c.expect(img[pstart * 512:pstart * 512 + 11] == esp[:11],
             "partition boot sector matches the UEFI El Torito ESP")

    print("UEFI FAT image")
    c.expect(esp[0] in (0xEB, 0xE9), "boot jump present")
    c.expect(u16(esp, 11) == 512, "512-byte sectors")
    spc = esp[13]
    c.expect(spc in (1, 2, 4, 8, 16, 32, 64, 128), f"{spc} sectors/cluster")
    res = u16(esp, 14)
    c.expect(res >= 1, f"{res} reserved sectors")
    c.expect(esp[16] >= 1, f"{esp[16]} FATs")
    rootent = u16(esp, 17)
    tot16 = u16(esp, 19)
    fatsz = u16(esp, 22)
    tot32 = u32(esp, 32)
    c.expect(tot16 != 0 and tot32 == 0,
             f"FAT16 size fields (TotSec16={tot16}, TotSec32={tot32})")
    root_secs = (rootent * 32 + 511) // 512
    data_start = res + esp[16] * fatsz + root_secs
    clusters = (tot16 - data_start) // spc if tot16 > data_start else 0
    c.expect(4085 <= clusters < 65525,
             f"{clusters} FAT16 clusters")

    loader = walk_fat(esp, "EFI/BOOT/BOOTX64.EFI")
    kernel = walk_fat(esp, "KERNEL.BIN")
    c.expect(loader is not None and len(loader) > 512,
             f"/EFI/BOOT/BOOTX64.EFI readable ({len(loader or b'')} bytes)")
    c.expect(kernel is not None and len(kernel) > 512,
             f"/KERNEL.BIN readable ({len(kernel or b'')} bytes)")

    print("ISO 9660 tree")
    iso_loader = walk_iso(img, "EFI/BOOT/BOOTX64.EFI")
    iso_kernel = walk_iso(img, "KERNEL.BIN")
    c.expect(iso_loader is not None,
             "/EFI/BOOT/BOOTX64.EFI present in the ISO tree")
    c.expect(iso_kernel is not None, "/KERNEL.BIN present in the ISO tree")
    c.expect(iso_loader == loader,
             "ISO tree loader matches the El Torito FAT image")
    c.expect(iso_kernel == kernel,
             "ISO tree kernel matches the El Torito FAT image")

    print()
    if c.failures:
        print(f"FAILED: {len(c.failures)} check(s)")
        for f in c.failures:
            print(f"  - {f}")
        return 1
    print(f"PASS: {c.passes} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
