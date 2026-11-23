#!/usr/bin/env python3
"""Link ELF64 relocatable objects into ReverticalOS RVLB/RVEX modules.

The on-disk containers are deliberately simple: a fixed 64-byte header, a
list of fields that must be rebased, a symbol table with per-symbol patch
lists, a string table and a flat section image linked at address zero.

    RVLB (.lib)  named exports for other modules
    RVEX (.rvx)  entry point, stack size and named imports

Version 2 is the x86-64 container.  Relocation and import-patch records carry
their width (8 or 4 bytes) so 64-bit pointers and the small-code-model
32-bit absolute references both survive being loaded at a nonzero base.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC_LIB = 0x424C5652   # "RVLB"
MAGIC_EXE = 0x58455652   # "RVEX"
VERSION = 2
KIND_LIB = 1
KIND_EXE = 2

SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9
SHT_RELA = 4
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2

R_X86_64_NONE = 0
R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_PLT32 = 4
R_X86_64_32 = 10
R_X86_64_32S = 11

HDR = struct.Struct("<IHHIIIIIII16sI8s")
SYM = struct.Struct("<IIII")
PATCH = struct.Struct("<IBBH")
assert HDR.size == 64 and SYM.size == 16 and PATCH.size == 8


class ElfError(Exception):
    pass


class Section:
    def __init__(self, index, name, typ, flags, offset, size, link, info,
                 align, entsize):
        self.index = index
        self.name = name
        self.type = typ
        self.flags = flags
        self.offset = offset
        self.size = size
        self.link = link
        self.info = info
        self.align = align
        self.entsize = entsize
        self.image_off = 0
        self.data = b""


class Symbol:
    def __init__(self, index, name, value, size, bind, typ, shndx):
        self.index = index
        self.name = name
        self.value = value
        self.size = size
        self.bind = bind
        self.type = typ
        self.shndx = shndx
        self.address = 0


class Object:
    def __init__(self, path: Path):
        data = path.read_bytes()
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ElfError(f"{path}: not a 64-bit little-endian ELF object")
        e_type = struct.unpack_from("<H", data, 16)[0]
        if e_type != 1:
            raise ElfError(f"{path}: expected a relocatable object (ET_REL), "
                           f"got {e_type}")
        e_shoff = struct.unpack_from("<Q", data, 40)[0]
        e_shentsize = struct.unpack_from("<H", data, 58)[0]
        e_shnum = struct.unpack_from("<H", data, 60)[0]
        e_shstrndx = struct.unpack_from("<H", data, 62)[0]
        if e_shentsize != 64:
            raise ElfError(f"{path}: unexpected section header size "
                           f"{e_shentsize}")

        raw = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            raw.append(struct.unpack_from("<IIQQQQIIQQ", data, off))

        shstr = raw[e_shstrndx]
        shstr_data = data[shstr[4]:shstr[4] + shstr[5]]

        def cstr(blob: bytes, off: int) -> str:
            end = blob.find(b"\0", off)
            return blob[off:end].decode("latin-1")

        self.sections = []
        for i, sh in enumerate(raw):
            name = cstr(shstr_data, sh[0]) if i else ""
            self.sections.append(Section(i, name, sh[1], sh[2], sh[4], sh[5],
                                         sh[6], sh[7], sh[8], sh[9]))

        self.symbols = []
        for sec in self.sections:
            if sec.type != SHT_SYMTAB:
                continue
            strtab = self.sections[sec.link]
            blob = data[strtab.offset:strtab.offset + strtab.size]
            for i in range(sec.size // 24):
                off = sec.offset + i * 24
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from("<IBBHQQ", data, off)
                self.symbols.append(Symbol(i, cstr(blob, st_name), st_value,
                                           st_size, st_info >> 4,
                                           st_info & 0xF, st_shndx))
        if not self.symbols:
            raise ElfError(f"{path}: object has no symbol table "
                           f"(compile without -s)")

    def symbol(self, index):
        return self.symbols[index]


# relocation kind -> (absolute?, width)
RELOC_KINDS = {
    R_X86_64_64: (True, 8),
    R_X86_64_32: (True, 4),
    R_X86_64_32S: (True, 4),
    R_X86_64_PC32: (False, 4),
    R_X86_64_PLT32: (False, 4),
}


def link(objects: list[Object], exports: list[str], kind: int,
         name: str, stack: int) -> bytes:
    # 1. Lay out allocated sections.
    sections = []
    for obj in objects:
        for sec in obj.sections:
            if sec.index == 0 or not (sec.flags & SHF_ALLOC):
                continue
            if sec.type == SHT_NOBITS:
                sec.data = b""
            else:
                sec.data = obj.data[sec.offset:sec.offset + sec.size]
                if len(sec.data) != sec.size:
                    raise ElfError(f"{sec.name}: truncated section")
            sections.append((obj, sec))

    image = bytearray()
    for obj, sec in sections:
        align = max(4, min(16, sec.align or 4))
        while len(image) % align:
            image.append(0)
        sec.image_off = len(image)
        image.extend(sec.data)
        if sec.type == SHT_NOBITS:
            image.extend(b"\0" * sec.size)

    # 2. Resolve symbol addresses for defined symbols.
    sec_map = {(id(obj), sec.index): sec for obj, sec in sections}
    for obj in objects:
        for sym in obj.symbols:
            if sym.shndx == SHN_ABS:
                sym.address = sym.value
            elif sym.shndx in (SHN_UNDEF, SHN_COMMON):
                sym.address = 0
            else:
                sec = sec_map.get((id(obj), sym.shndx))
                # A symbol defined in a non-allocated section (debug/comment)
                # is not usable at run time.
                sym.address = 0 if sec is None else sec.image_off + sym.value

    # 3. Apply relocations.
    relocs: list[tuple[int, int]] = []
    imports: dict[str, list[tuple[int, int, int]]] = {}

    for obj in objects:
        for rsec in obj.sections:
            if rsec.type == SHT_REL:
                raise ElfError(
                    f"{rsec.name}: RELA relocations are required for x86-64")
            if rsec.type != SHT_RELA:
                continue
            target = sec_map.get((id(obj), rsec.info))
            if target is None:
                continue
            for i in range(rsec.size // 24):
                off = rsec.offset + i * 24
                r_offset, r_info, addend = struct.unpack_from("<QQq",
                                                              obj.data, off)
                rtype = r_info & 0xFFFFFFFF
                sym = obj.symbol(r_info >> 32)
                if rtype == R_X86_64_NONE:
                    continue
                if rtype not in RELOC_KINDS:
                    raise ElfError(f"{sym.name}: unsupported relocation "
                                   f"{rtype}")
                absolute, width = RELOC_KINDS[rtype]
                pos = target.image_off + r_offset
                if pos + width > len(image):
                    raise ElfError(f"{obj}: relocation outside the image")

                if sym.shndx in (SHN_UNDEF, SHN_COMMON):
                    # Store the addend now; the loader folds in the address.
                    _put(image, pos, addend, width)
                    ptype = 1 if absolute else 2
                    imports.setdefault(sym.name, []).append((pos, ptype, width))
                    continue

                if absolute:
                    value = sym.address + addend
                    _put(image, pos, value, width)
                    if sym.shndx != SHN_ABS:
                        relocs.append((pos, width))
                else:
                    value = sym.address + addend - pos
                    _put(image, pos, value, width)

    # 4. Check the requested exports.
    export_values: list[tuple[str, int]] = []
    for wanted in exports:
        found = False
        for obj in objects:
            for sym in obj.symbols:
                if sym.name == wanted and sym.shndx not in (SHN_UNDEF,
                                                           SHN_COMMON):
                    export_values.append((wanted, sym.address))
                    found = True
                    break
            if found:
                break
        if not found:
            raise ElfError(f"export {wanted!r} is not defined")

    if kind == KIND_LIB and imports:
        raise ElfError("libraries must be self-contained; unresolved: " +
                       ", ".join(sorted(imports)))
    if kind == KIND_EXE:
        entry = 0
        found_entry = False
        for obj in objects:
            for sym in obj.symbols:
                if sym.name == "_start" and sym.shndx not in (SHN_UNDEF,
                                                             SHN_COMMON):
                    entry = sym.address
                    found_entry = True
        if not found_entry:
            raise ElfError("executable has no _start")
        if entry >= len(image):
            raise ElfError("_start is outside the image")
    else:
        entry = 0

    # 5. Build strings, symbols and patch tables.
    strings = bytearray()
    str_offsets: dict[str, int] = {}

    def string_off(text: str) -> int:
        if text in str_offsets:
            return str_offsets[text]
        off = len(strings)
        strings.extend(text.encode("latin-1") + b"\0")
        str_offsets[text] = off
        return off

    sym_entries = []
    if kind == KIND_LIB:
        for sname, value in export_values:
            sym_entries.append((string_off(sname), value, 0, 0))
    else:
        for sname, patches in imports.items():
            string_off(sname)
            sym_entries.append([string_off(sname), 0, len(patches), 0])

    reloc_off = HDR.size
    sym_off = reloc_off + PATCH.size * len(relocs)
    str_off = sym_off + SYM.size * len(sym_entries)
    patch_off = str_off + len(strings)

    patch_blob = bytearray()
    if kind == KIND_EXE:
        for se, (sname, patches) in zip(sym_entries, imports.items()):
            se[3] = patch_off + len(patch_blob)
            for pos, ptype, width in patches:
                patch_blob.extend(PATCH.pack(pos, ptype, width, 0))
        cursor = patch_off + len(patch_blob)
    else:
        cursor = patch_off

    image_off = (cursor + 15) & ~15
    checksum = sum(image) & 0xFFFFFFFF

    header = HDR.pack(MAGIC_LIB if kind == KIND_LIB else MAGIC_EXE,
                      VERSION, kind, len(image), entry, stack,
                      len(relocs), len(sym_entries), len(strings),
                      image_off, name.encode("latin-1")[:15].ljust(16, b"\0"),
                      checksum, b"\0" * 8)

    out = bytearray(header)
    for pos, width in relocs:
        out.extend(PATCH.pack(pos, 1, width, 0))
    for entry in sym_entries:
        out.extend(SYM.pack(*entry))
    out.extend(strings)
    out.extend(patch_blob)
    while len(out) < image_off:
        out.append(0)
    out.extend(image)
    return bytes(out)


def _put(image: bytearray, pos: int, value: int, width: int) -> None:
    if width == 8:
        struct.pack_into("<Q", image, pos, value & 0xFFFFFFFFFFFFFFFF)
    else:
        struct.pack_into("<I", image, pos, value & 0xFFFFFFFF)


def inspect(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < HDR.size:
        print(f"{path}: too short", file=sys.stderr)
        return 1
    (magic, version, kind, image_size, entry, stack, reloc_count, sym_count,
     str_size, image_off, name, checksum, _) = HDR.unpack_from(data)
    if magic not in (MAGIC_LIB, MAGIC_EXE):
        print(f"{path}: bad magic {magic:08X}", file=sys.stderr)
        return 1
    print(f"{path}: {'RVLB library' if magic == MAGIC_LIB else 'RVEX executable'} "
          f"v{version} name={name.rstrip(chr(0).encode()).decode()} "
          f"image={image_size} entry=0x{entry:x} stack={stack} "
          f"relocs={reloc_count} symbols={sym_count}")
    sym_off = HDR.size + reloc_count * PATCH.size
    str_off = sym_off + sym_count * SYM.size
    patch_off = str_off + str_size
    strings = data[str_off:str_off + str_size]
    actual = sum(data[image_off:image_off + image_size]) & 0xFFFFFFFF
    print(f"  checksum 0x{checksum:08X} "
          f"({'ok' if actual == checksum else 'MISMATCH'})")
    for i in range(sym_count):
        name_off, value, patch_count, patch_off_s = SYM.unpack_from(
            data, sym_off + i * SYM.size)
        end = strings.find(b"\0", name_off)
        sname = strings[name_off:end].decode("latin-1")
        if patch_count:
            kinds = []
            for p in range(patch_count):
                off, ptype, width, _ = PATCH.unpack_from(
                    data, patch_off_s + p * PATCH.size)
                kinds.append(f"0x{off:x}{'abs' if ptype == 1 else 'rel'}"
                             f"{width * 8}")
            print(f"  import {sname!r}: {', '.join(kinds)}")
        else:
            print(f"  export {sname!r} at +0x{value:x}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="RVLB/RVEX linker")
    sub = ap.add_subparsers(dest="cmd", required=True)

    build = sub.add_parser("lib", help="link an RVLB library")
    build.add_argument("--object", action="append", required=True)
    build.add_argument("--export", action="append", default=[])
    build.add_argument("--name", required=True)
    build.add_argument("--output", required=True)

    exe = sub.add_parser("exe", help="link an RVEX executable")
    exe.add_argument("--object", action="append", required=True)
    exe.add_argument("--name", required=True)
    exe.add_argument("--stack", type=lambda v: int(v, 0), default=65536)
    exe.add_argument("--output", required=True)

    ins = sub.add_parser("inspect", help="dump a module header and symbols")
    ins.add_argument("module")

    args = ap.parse_args()

    if args.cmd == "inspect":
        return inspect(Path(args.module))

    try:
        objects = [Object(Path(p)) for p in args.object]
        if args.cmd == "lib":
            data = link(objects, args.export, KIND_LIB, args.name, 0)
        else:
            data = link(objects, [], KIND_EXE, args.name, args.stack)
    except ElfError as exc:
        print(f"rvlink: {exc}", file=sys.stderr)
        return 1

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"rvlink: {out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
