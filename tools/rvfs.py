#!/usr/bin/env python3
"""RVFS - Revertical File System host tool.

Creates, populates and inspects RVFS volumes. The on-disk layout here is the
same one kernel/rvfs.c implements:

    block 0                 superblock
    blocks 1..inode_blocks  inode table (64 byte inodes)
    block  bitmap           block bitmap
    block  inode bitmap     inode bitmap
    ...                     data blocks

Block size is 4096 bytes. Inodes have 12 direct blocks plus one single
indirect block (up to 4 MiB + 48 KiB per file).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

BLOCK = 4096
MAGIC = 0x53465652  # "RVFS"
VERSION = 1
INODE_SIZE = 64
INODES_PER_BLOCK = BLOCK // INODE_SIZE
DIRECT = 12
PTRS_PER_BLOCK = BLOCK // 4
INODE_COUNT = 1024
DIR_HEADER = 8
NAME_MAX = 63

MODE_FREE = 0
MODE_FILE = 1
MODE_DIR = 2


class Inode:
    def __init__(self, mode=MODE_FREE):
        self.mode = mode
        self.links = 1
        self.size = 0
        self.mtime = 0
        self.blocks = [0] * DIRECT
        self.indirect = 0

    def pack(self) -> bytes:
        raw = struct.pack("<HHI", self.mode, self.links, self.size)
        raw += struct.pack("<I", self.mtime)
        raw += b"".join(struct.pack("<I", b) for b in self.blocks)
        raw += struct.pack("<I", self.indirect)
        assert len(raw) == INODE_SIZE
        return raw


def dir_entry(name: str, ino: int, kind: int) -> bytes:
    name_bytes = name.encode("utf-8")
    if len(name_bytes) > NAME_MAX:
        raise ValueError(f"name too long: {name}")
    need = DIR_HEADER + len(name_bytes)
    need = (need + 3) & ~3
    raw = struct.pack("<IBBH", ino, kind, len(name_bytes), 0)
    raw += name_bytes
    return raw + b"\0" * (need - len(raw))


def split_path(path: str) -> list[str]:
    return [p for p in path.strip("/").split("/") if p]


class Volume:
    def __init__(self, total_blocks: int, label: str):
        if total_blocks < 32:
            raise ValueError("volume too small")
        self.total_blocks = total_blocks
        self.label = label
        self.inodes = [Inode() for _ in range(INODE_COUNT)]
        self.blocks_data = {}
        self.used = bytearray(total_blocks)
        self.inode_used = bytearray(INODE_COUNT)
        self.inode_blocks = (INODE_COUNT + INODES_PER_BLOCK - 1) // INODES_PER_BLOCK
        self.block_bitmap_block = 1 + self.inode_blocks
        self.block_bitmap_blocks = (total_blocks + BLOCK * 8 - 1) // (BLOCK * 8)
        self.inode_bitmap_block = self.block_bitmap_block + self.block_bitmap_blocks
        self.inode_bitmap_blocks = (INODE_COUNT + BLOCK * 8 - 1) // (BLOCK * 8)
        self.data_start = self.inode_bitmap_block + self.inode_bitmap_blocks
        for b in range(self.data_start):
            self.used[b] = 1

        self.root = self._alloc_inode(MODE_DIR, link=True)
        assert self.root == 1
        self._write_dir(self.root, [(".", self.root, MODE_DIR),
                                    ("..", self.root, MODE_DIR)])
        self.dirs: dict[str, int] = {"/": self.root}
        self.files: dict[str, int] = {}

    # -- allocation -------------------------------------------------------
    def _alloc_block(self) -> int:
        for b in range(self.data_start, self.total_blocks):
            if not self.used[b]:
                self.used[b] = 1
                return b
        raise RuntimeError("volume full")

    def _alloc_inode(self, mode: int, link=False) -> int:
        for i in range(1, INODE_COUNT):
            if not self.inode_used[i]:
                self.inode_used[i] = 1
                self.inodes[i] = Inode(mode)
                if mode == MODE_DIR:
                    self.inodes[i].links = 2 if link else 1
                return i
        raise RuntimeError("out of inodes")

    # -- low level --------------------------------------------------------
    def _write_at(self, ino: int, off: int, data: bytes):
        node = self.inodes[ino]
        if node.mode == MODE_FILE or node.mode == MODE_DIR:
            pass
        pos = 0
        while pos < len(data):
            block_index = (off + pos) // BLOCK
            block_off = (off + pos) % BLOCK
            chunk = min(BLOCK - block_off, len(data) - pos)
            blk = self._file_block(ino, block_index, alloc=True)
            segment = self._block_bytes(blk)
            segment[block_off:block_off + chunk] = data[pos:pos + chunk]
            self.blocks_data[blk] = bytes(segment)
            pos += chunk
        end = off + len(data)
        if end > node.size:
            node.size = end

    def _file_block(self, ino: int, index: int, alloc: bool) -> int:
        node = self.inodes[ino]
        if index < DIRECT:
            if node.blocks[index] == 0 and alloc:
                node.blocks[index] = self._alloc_block()
            return node.blocks[index]
        index -= DIRECT
        if index >= PTRS_PER_BLOCK:
            raise RuntimeError("file too large")
        if node.indirect == 0 and alloc:
            node.indirect = self._alloc_block()
            self.blocks_data[node.indirect] = b"\0" * BLOCK
        if node.indirect == 0:
            return 0
        table = bytearray(self.blocks_data[node.indirect])
        ptr = struct.unpack_from("<I", table, index * 4)[0]
        if ptr == 0 and alloc:
            ptr = self._alloc_block()
            struct.pack_into("<I", table, index * 4, ptr)
            self.blocks_data[node.indirect] = bytes(table)
        return ptr

    def _block_bytes(self, blk: int) -> bytearray:
        if blk == 0:
            return bytearray(BLOCK)
        return bytearray(self.blocks_data.get(blk, b"\0" * BLOCK))

    def _write_dir(self, ino: int, entries: list[tuple[str, int, int]]):
        data = b"".join(dir_entry(name, child, kind)
                        for name, child, kind in entries)
        self._write_at(ino, 0, data)
        self.inodes[ino].size = len(data)

    # -- public -----------------------------------------------------------
    def mkdir(self, path: str) -> int:
        path = "/" + path.strip("/")
        if path in self.dirs:
            return self.dirs[path]
        parent_path, name = path.rsplit("/", 1)
        parent = self.mkdir(parent_path)
        ino = self._alloc_inode(MODE_DIR)
        entries = self._read_dir_entries(parent)
        if any(e[0] == name for e in entries):
            raise RuntimeError(f"already exists: {path}")
        entries.append((name, ino, MODE_DIR))
        self._write_dir(parent, entries)
        self._write_dir(ino, [(".", ino, MODE_DIR), ("..", parent, MODE_DIR)])
        self.dirs[path] = ino
        return ino

    def add_file(self, path: str, data: bytes) -> int:
        path = "/" + path.strip("/")
        parent_path, name = path.rsplit("/", 1)
        parent = self.mkdir(parent_path)
        entries = self._read_dir_entries(parent)
        if any(e[0] == name for e in entries):
            raise RuntimeError(f"already exists: {path}")
        ino = self._alloc_inode(MODE_FILE)
        self.inodes[ino].size = 0
        self._write_at(ino, 0, data)
        self.inodes[ino].size = len(data)
        entries.append((name, ino, MODE_FILE))
        self._write_dir(parent, entries)
        self.files[path] = ino
        return ino

    def _read_dir_entries(self, ino: int) -> list[tuple[str, int, int]]:
        raw = self.read_file(ino)
        entries = []
        pos = 0
        while pos + DIR_HEADER <= len(raw):
            child, kind, nlen, _ = struct.unpack_from("<IBBH", raw, pos)
            need = (DIR_HEADER + nlen + 3) & ~3
            if nlen == 0 or pos + need > len(raw):
                break
            name = raw[pos + DIR_HEADER:pos + DIR_HEADER + nlen].decode("utf-8")
            if child:
                entries.append((name, child, kind))
            pos += need
        return entries

    def read_file(self, ino: int) -> bytes:
        node = self.inodes[ino]
        out = bytearray()
        remaining = node.size
        index = 0
        while remaining > 0:
            blk = self._file_block(ino, index, alloc=False)
            if blk == 0:
                out += b"\0" * min(BLOCK, remaining)
            else:
                out += self._block_bytes(blk)[:min(BLOCK, remaining)]
            remaining -= min(BLOCK, remaining)
            index += 1
        return bytes(out)

    # -- serialisation ----------------------------------------------------
    def build(self) -> bytes:
        free_blocks = sum(1 for b in range(self.data_start, self.total_blocks)
                          if not self.used[b])
        free_inodes = sum(1 for i in range(1, INODE_COUNT)
                          if not self.inode_used[i])

        out = bytearray(self.total_blocks * BLOCK)

        sb = struct.pack(
            "<IIIIIIIIIIIIII32s40s",
            MAGIC, VERSION, BLOCK, self.total_blocks,
            INODE_COUNT, self.inode_blocks,
            self.block_bitmap_block, self.block_bitmap_blocks,
            self.inode_bitmap_block, self.inode_bitmap_blocks,
            self.data_start, self.root, free_blocks, free_inodes,
            self.label.encode("utf-8")[:31], b"",
        )
        out[0:len(sb)] = sb

        inode_raw = bytearray(self.inode_blocks * BLOCK)
        for i in range(1, INODE_COUNT):
            off = (i - 1) * INODE_SIZE
            inode_raw[off:off + INODE_SIZE] = self.inodes[i].pack()
        inode_off = 1 * BLOCK
        out[inode_off:inode_off + len(inode_raw)] = inode_raw

        bmap = bytearray(self.block_bitmap_blocks * BLOCK)
        for b in range(self.total_blocks):
            if self.used[b]:
                bmap[b // 8] |= 1 << (b % 8)
        boff = self.block_bitmap_block * BLOCK
        out[boff:boff + len(bmap)] = bmap

        imap = bytearray(self.inode_bitmap_blocks * BLOCK)
        for i in range(INODE_COUNT):
            if self.inode_used[i]:
                imap[i // 8] |= 1 << (i % 8)
        ioff = self.inode_bitmap_block * BLOCK
        out[ioff:ioff + len(imap)] = imap

        for blk, data in self.blocks_data.items():
            out[blk * BLOCK:blk * BLOCK + len(data)] = data
        return bytes(out)


def read_volume(data: bytes) -> Volume:
    (magic, version, block_size, total_blocks,
     inode_count, inode_blocks,
     bb_block, bb_blocks, ib_block, ib_blocks,
     data_start, root_ino, free_blocks, free_inodes,
     label, _) = struct.unpack_from("<IIIIIIIIIIIIII32s40s", data, 0)
    if magic != MAGIC:
        raise ValueError("not an RVFS volume")

    vol = Volume.__new__(Volume)
    vol.total_blocks = total_blocks
    vol.label = label.split(b"\0")[0].decode("utf-8", "replace")
    vol.inode_blocks = inode_blocks
    vol.block_bitmap_block = bb_block
    vol.block_bitmap_blocks = bb_blocks
    vol.inode_bitmap_block = ib_block
    vol.inode_bitmap_blocks = ib_blocks
    vol.data_start = data_start
    vol.used = bytearray(total_blocks)
    for b in range(total_blocks):
        if data[bb_block * BLOCK + b // 8] & (1 << (b % 8)):
            vol.used[b] = 1
    vol.inode_used = bytearray(inode_count)
    for i in range(inode_count):
        if data[ib_block * BLOCK + i // 8] & (1 << (i % 8)):
            vol.inode_used[i] = 1
    inodes = [Inode()]
    for i in range(inode_count):
        off = BLOCK + i * INODE_SIZE
        mode, links, size = struct.unpack_from("<HHI", data, off)
        node = Inode(mode)
        node.links = links
        node.size = size
        node.mtime = struct.unpack_from("<I", data, off + 8)[0]
        node.blocks = list(struct.unpack_from("<12I", data, off + 12))
        node.indirect = struct.unpack_from("<I", data, off + 60)[0]
        inodes.append(node)
    vol.inodes = inodes
    vol.blocks_data = {b: bytes(data[b * BLOCK:(b + 1) * BLOCK])
                       for b in range(data_start, total_blocks)
                       if vol.used[b]}
    vol.dirs = {"/": root_ino}
    vol.files = {}
    return vol


def walk(vol: Volume, ino=None, prefix=""):
    if ino is None:
        ino = 1
    for name, child, kind in sorted(vol._read_dir_entries(ino)):
        if name in (".", ".."):
            continue
        path = f"{prefix}/{name}" if prefix else name
        yield path, child, kind
        if kind == MODE_DIR:
            yield from walk(vol, child, path)


def create_volume(total_sectors: int, label: str,
                  files: dict[str, bytes] | None = None) -> bytes:
    total_blocks = total_sectors * 512 // BLOCK
    vol = Volume(total_blocks, label)
    vol.blocks_data = {}
    for path, data in (files or {}).items():
        vol.add_file(path, data)
    return vol.build()


def main() -> int:
    ap = argparse.ArgumentParser(description="RVFS host tool")
    ap.add_argument("command", choices=["list", "check", "mkfs"])
    ap.add_argument("image", help="raw disk image or RVFS volume")
    ap.add_argument("--offset", type=int, default=0,
                    help="byte offset of the volume (0 = scan the MBR)")
    ap.add_argument("--size-mb", type=int, default=28)
    ap.add_argument("--label", default="ReverticalOS")
    args = ap.parse_args()

    raw = Path(args.image).read_bytes()
    off = args.offset
    if off == 0 and raw[510:512] == b"\x55\xaa":
        entries = []
        for i in range(4):
            e = raw[446 + i * 16:446 + (i + 1) * 16]
            start, count = struct.unpack_from("<II", e, 8)
            if e[4] and count:
                entries.append((e[4], start, count))
        if not entries:
            print("no partitions in MBR", file=sys.stderr)
            return 1
        ptype, start, count = entries[0]
        print(f"partition type 0x{ptype:02X} start {start} sectors {count}")
        off = start * 512

    if args.command == "mkfs":
        blob = create_volume(args.size_mb * 1024 * 1024 // 512, args.label)
        Path(args.image).write_bytes(blob)
        print(f"wrote {len(blob)} bytes")
        return 0

    vol = read_volume(raw[off:])
    print(f"label: {vol.label}")
    print(f"blocks: {vol.total_blocks}  data start: {vol.data_start}")
    print(f"free blocks: {sum(1 for b in range(vol.data_start, vol.total_blocks) if not vol.used[b])}")
    if args.command == "check":
        bad = 0
        for path, ino, kind in walk(vol):
            node = vol.inodes[ino]
            if kind == MODE_FILE:
                size = node.size
                if size > (DIRECT + PTRS_PER_BLOCK) * BLOCK:
                    print(f"  bad size {path}")
                    bad += 1
            print(f"  {'d' if kind == MODE_DIR else 'f'} {path}  {node.size}")
        print("ok" if not bad else f"{bad} problems")
        return 1 if bad else 0
    for path, ino, kind in walk(vol):
        node = vol.inodes[ino]
        print(f"  {'d' if kind == MODE_DIR else 'f'} {path:38s} {node.size:8d} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
