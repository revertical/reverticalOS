/* RVFS - the Revertical File System.
 *
 * A small inode/bitmap filesystem on top of the ATA driver. Block size is
 * 4096 bytes, inodes are 64 bytes and carry 12 direct block pointers plus one
 * single-indirect block. The on-disk format is mirrored by tools/rvfs.py.
 */
#include "rvfs.h"
#include "blk.h"
#include "serial.h"
#include "string.h"

#define RVFS_MAGIC        0x53465652u   /* "RVFS" */
#define RVFS_VERSION      1u
#define RVFS_BLOCK        4096u
#define RVFS_SECTORS      8u            /* 512 byte sectors per block */
#define RVFS_INODE_SIZE   64u
#define RVFS_INODES_BLK   64u
#define RVFS_DIRECT       12u
#define RVFS_PTRS         (RVFS_BLOCK / 4u)
#define RVFS_NAME_LEN     63u
#define RVFS_MAX_BLOCKS   (2u * 1024u * 1024u * 1024u / RVFS_BLOCK)
#define RVFS_MAX_BITMAP   16u           /* 64 KiB of block bitmap */
#define RVFS_DIR_MAX      (16u * 1024u)

#define INODE_FREE 0
#define INODE_FILE 1
#define INODE_DIR  2

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 block_size;
    u32 total_blocks;
    u32 inode_count;
    u32 inode_blocks;
    u32 block_bitmap_block;
    u32 block_bitmap_blocks;
    u32 inode_bitmap_block;
    u32 inode_bitmap_blocks;
    u32 data_start;
    u32 root_ino;
    u32 free_blocks;
    u32 free_inodes;
    char label[32];
    u8 reserved[40];
} RvSuper;

typedef struct __attribute__((packed)) {
    u16 mode;
    u16 links;
    u32 size;
    u32 mtime;
    u32 block[RVFS_DIRECT];
    u32 indirect;
} RvInode;

typedef struct __attribute__((packed)) {
    u32 ino;
    u8 type;
    u8 name_len;
    u16 reserved;
} RvDirHeader;

static DiskPart vol_part;
static int mounted;
static RvSuper sb;
static u8 block_bitmap[RVFS_MAX_BITMAP * RVFS_BLOCK];
static u8 inode_bitmap[RVFS_BLOCK];
static u8 blk_buf[RVFS_BLOCK];
static u8 dir_buf[RVFS_DIR_MAX];
static RvfsVolume vol_info;
static int dirty_sb;

/* ------------------------------------------------------------------ */
/*  Block I/O                                                         */
/* ------------------------------------------------------------------ */
static int rvblk_read(u32 blk, void *buf)
{
    if (blk >= sb.total_blocks)
        return RVFS_EIO;
    return disk_read_part(&vol_part, blk * RVFS_SECTORS, RVFS_SECTORS, buf)
               ? RVFS_EIO : RVFS_OK;
}

static int rvblk_write(u32 blk, const void *buf)
{
    if (blk >= sb.total_blocks)
        return RVFS_EIO;
    return disk_write_part(&vol_part, blk * RVFS_SECTORS, RVFS_SECTORS, buf)
               ? RVFS_EIO : RVFS_OK;
}

static int map_flush(void)
{
    int rc = RVFS_OK;
    for (u32 i = 0; i < sb.block_bitmap_blocks; i++) {
        int r = rvblk_write(sb.block_bitmap_block + i,
                          block_bitmap + i * RVFS_BLOCK);
        if (r) rc = r;
    }
    for (u32 i = 0; i < sb.inode_bitmap_blocks; i++) {
        int r = rvblk_write(sb.inode_bitmap_block + i,
                          inode_bitmap + i * RVFS_BLOCK);
        if (r) rc = r;
    }
    return rc;
}

static int sb_flush(void)
{
    if (!dirty_sb)
        return RVFS_OK;
    sb.free_blocks = 0;
    for (u32 b = sb.data_start; b < sb.total_blocks; b++)
        if (!(block_bitmap[b >> 3] & (1u << (b & 7))))
            sb.free_blocks++;
    dirty_sb = 0;
    memcpy(blk_buf, &sb, sizeof(sb));
    return rvblk_write(0, blk_buf);
}

static int bit_get(const u8 *map, u32 bit)
{
    return (map[bit >> 3] >> (bit & 7)) & 1;
}

static void bit_set(u8 *map, u32 bit, int value)
{
    if (value)
        map[bit >> 3] |= (u8)(1u << (bit & 7));
    else
        map[bit >> 3] &= (u8)~(1u << (bit & 7));
}

/* ------------------------------------------------------------------ */
/*  Inodes                                                            */
/* ------------------------------------------------------------------ */
static int inode_read(u32 ino, RvInode *out)
{
    if (ino < 1 || ino > sb.inode_count)
        return RVFS_EINVAL;
    u32 idx = ino - 1;
    u32 blk = 1 + idx / RVFS_INODES_BLK;
    u32 off = (idx % RVFS_INODES_BLK) * RVFS_INODE_SIZE;
    int r = rvblk_read(blk, blk_buf);
    if (r) return r;
    memcpy(out, blk_buf + off, sizeof(RvInode));
    return RVFS_OK;
}

static int inode_write(u32 ino, const RvInode *in)
{
    if (ino < 1 || ino > sb.inode_count)
        return RVFS_EINVAL;
    u32 idx = ino - 1;
    u32 blk = 1 + idx / RVFS_INODES_BLK;
    u32 off = (idx % RVFS_INODES_BLK) * RVFS_INODE_SIZE;
    int r = rvblk_read(blk, blk_buf);
    if (r) return r;
    memcpy(blk_buf + off, in, sizeof(RvInode));
    return rvblk_write(blk, blk_buf);
}

static int alloc_block(u32 *out)
{
    for (u32 b = sb.data_start; b < sb.total_blocks; b++) {
        if (!bit_get(block_bitmap, b)) {
            bit_set(block_bitmap, b, 1);
            dirty_sb = 1;
            *out = b;
            return map_flush();
        }
    }
    return RVFS_ENOSPC;
}

static int free_block(u32 blk)
{
    if (blk < sb.data_start || blk >= sb.total_blocks)
        return RVFS_EINVAL;
    bit_set(block_bitmap, blk, 0);
    dirty_sb = 1;
    return RVFS_OK;
}

static int alloc_inode(u32 *out)
{
    for (u32 i = 1; i < sb.inode_count; i++) {
        if (!bit_get(inode_bitmap, i)) {
            bit_set(inode_bitmap, i, 1);
            dirty_sb = 1;
            *out = i;
            return map_flush();
        }
    }
    return RVFS_ENOSPC;
}

static int free_inode(u32 ino)
{
    if (ino < 1 || ino > sb.inode_count)
        return RVFS_EINVAL;
    bit_set(inode_bitmap, ino, 0);
    dirty_sb = 1;
    return RVFS_OK;
}

static int file_block(RvInode *in, u32 index, int alloc, u32 *out)
{
    if (index < RVFS_DIRECT) {
        if (in->block[index] == 0 && alloc) {
            int r = alloc_block(&in->block[index]);
            if (r) return r;
        }
        *out = in->block[index];
        return RVFS_OK;
    }

    index -= RVFS_DIRECT;
    if (index >= RVFS_PTRS)
        return RVFS_EFBIG;

    if (in->indirect == 0 && alloc) {
        u32 blk;
        int r = alloc_block(&blk);
        if (r) return r;
        memset(blk_buf, 0, RVFS_BLOCK);
        r = rvblk_write(blk, blk_buf);
        if (r) return r;
        in->indirect = blk;
    }
    if (in->indirect == 0) {
        *out = 0;
        return RVFS_OK;
    }

    int r = rvblk_read(in->indirect, blk_buf);
    if (r) return r;
    u32 ptr;
    memcpy(&ptr, blk_buf + index * 4, 4);
    if (ptr == 0 && alloc) {
        u32 blk;
        r = alloc_block(&blk);
        if (r) return r;
        ptr = blk;
        memcpy(blk_buf + index * 4, &ptr, 4);
        r = rvblk_write(in->indirect, blk_buf);
        if (r) return r;
    }
    *out = ptr;
    return RVFS_OK;
}

static int inode_read_data(const RvInode *in, u32 offset, void *dst, u32 len)
{
    u8 *out = dst;
    while (len) {
        u32 index = offset / RVFS_BLOCK;
        u32 inner = offset % RVFS_BLOCK;
        u32 chunk = RVFS_BLOCK - inner;
        if (chunk > len) chunk = len;

        RvInode tmp = *in;
        u32 blk = 0;
        int r = file_block(&tmp, index, 0, &blk);
        if (r) return r;
        if (blk == 0) {
            memset(out, 0, chunk);
        } else {
            r = rvblk_read(blk, blk_buf);
            if (r) return r;
            memcpy(out, blk_buf + inner, chunk);
        }
        out += chunk;
        offset += chunk;
        len -= chunk;
    }
    return RVFS_OK;
}

static int inode_write_data(u32 ino, RvInode *in, u32 offset, const void *src,
                            u32 len)
{
    const u8 *p = src;
    while (len) {
        u32 index = offset / RVFS_BLOCK;
        u32 inner = offset % RVFS_BLOCK;
        u32 chunk = RVFS_BLOCK - inner;
        if (chunk > len) chunk = len;

        u32 blk;
        int r = file_block(in, index, 1, &blk);
        if (r) return r;
        if (inner == 0) {
            memset(blk_buf, 0, RVFS_BLOCK);
        } else {
            r = rvblk_read(blk, blk_buf);
            if (r) return r;
        }
        memcpy(blk_buf + inner, p, chunk);
        r = rvblk_write(blk, blk_buf);
        if (r) return r;

        p += chunk;
        offset += chunk;
        len -= chunk;
    }
    if (offset > in->size)
        in->size = offset;
    return inode_write(ino, in);
}

static int free_inode_data(RvInode *in)
{
    for (u32 i = 0; i < RVFS_DIRECT; i++)
        if (in->block[i]) {
            free_block(in->block[i]);
            in->block[i] = 0;
        }
    if (in->indirect) {
        if (rvblk_read(in->indirect, blk_buf) == RVFS_OK) {
            for (u32 i = 0; i < RVFS_PTRS; i++) {
                u32 ptr;
                memcpy(&ptr, blk_buf + i * 4, 4);
                if (ptr) free_block(ptr);
            }
        }
        free_block(in->indirect);
        in->indirect = 0;
    }
    return map_flush();
}

/* ------------------------------------------------------------------ */
/*  Directories                                                       */
/* ------------------------------------------------------------------ */
static u32 dir_entry_size(u32 name_len)
{
    return (u32)((sizeof(RvDirHeader) + name_len + 3u) & ~3u);
}

static int dir_load(u32 ino, u32 *size)
{
    RvInode in;
    int r = inode_read(ino, &in);
    if (r) return r;
    if (in.mode != INODE_DIR)
        return RVFS_ENOTDIR;
    if (in.size > RVFS_DIR_MAX)
        return RVFS_EFBIG;
    *size = in.size;
    if (in.size == 0)
        return RVFS_OK;
    return inode_read_data(&in, 0, dir_buf, in.size);
}

static int dir_lookup_ino(u32 dir_ino, const char *name, u32 *out_ino,
                          u8 *out_type)
{
    u32 size;
    int r = dir_load(dir_ino, &size);
    if (r) return r;

    u32 pos = 0;
    while (pos + sizeof(RvDirHeader) <= size) {
        RvDirHeader h;
        memcpy(&h, dir_buf + pos, sizeof(h));
        u32 need = dir_entry_size(h.name_len);
        if (h.name_len == 0 || pos + need > size)
            break;
        if (h.ino != 0 && h.name_len == strlen(name) &&
            memcmp(dir_buf + pos + sizeof(h), name, h.name_len) == 0) {
            *out_ino = h.ino;
            if (out_type) *out_type = h.type;
            return RVFS_OK;
        }
        pos += need;
    }
    return RVFS_ENOENT;
}

static int dir_add(u32 dir_ino, const char *name, u32 child, u8 type)
{
    u32 size;
    int r = dir_load(dir_ino, &size);
    if (r) return r;

    u32 name_len = strlen(name);
    if (name_len == 0 || name_len > RVFS_NAME_LEN)
        return RVFS_EINVAL;
    u32 existing;
    if (dir_lookup_ino(dir_ino, name, &existing, 0) == RVFS_OK)
        return RVFS_EEXIST;

    u32 need = dir_entry_size(name_len);
    if (size + need > RVFS_DIR_MAX)
        return RVFS_EFBIG;

    RvDirHeader h;
    h.ino = child;
    h.type = type;
    h.name_len = (u8)name_len;
    h.reserved = 0;
    memset(dir_buf + size, 0, need);
    memcpy(dir_buf + size, &h, sizeof(h));
    memcpy(dir_buf + size + sizeof(h), name, name_len);

    RvInode in;
    r = inode_read(dir_ino, &in);
    if (r) return r;
    return inode_write_data(dir_ino, &in, size, dir_buf + size, need);
}

static int dir_remove(u32 dir_ino, const char *name)
{
    u32 size;
    int r = dir_load(dir_ino, &size);
    if (r) return r;

    u32 pos = 0;
    while (pos + sizeof(RvDirHeader) <= size) {
        RvDirHeader h;
        memcpy(&h, dir_buf + pos, sizeof(h));
        u32 need = dir_entry_size(h.name_len);
        if (h.name_len == 0 || pos + need > size)
            break;
        if (h.ino != 0 && h.name_len == strlen(name) &&
            memcmp(dir_buf + pos + sizeof(h), name, h.name_len) == 0) {
            u32 zero = 0;
            RvInode in;
            r = inode_read(dir_ino, &in);
            if (r) return r;
            return inode_write_data(dir_ino, &in, pos, &zero, 4);
        }
        pos += need;
    }
    return RVFS_ENOENT;
}

/* ------------------------------------------------------------------ */
/*  Paths                                                             */
/* ------------------------------------------------------------------ */
static int split_parent(const char *path, char *parent, char *name)
{
    if (!path || path[0] != '/')
        return RVFS_EINVAL;
    const char *last = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p;
    if (!last)
        return RVFS_EINVAL;
    u32 plen = (u32)(last - path);
    if (plen == 0)
        plen = 1;
    if (plen >= 128)
        return RVFS_EINVAL;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    if (plen > 1 && parent[plen - 1] == '/')
        parent[plen - 1] = 0;

    const char *n = last + 1;
    u32 nlen = strlen(n);
    if (nlen == 0 || nlen > RVFS_NAME_LEN)
        return RVFS_EINVAL;
    memcpy(name, n, nlen + 1);
    return RVFS_OK;
}

static int resolve(const char *path, u32 *out_ino, u8 *out_type)
{
    if (!mounted)
        return RVFS_EIO;
    if (!path || path[0] != '/')
        return RVFS_EINVAL;

    u32 cur = sb.root_ino;
    u8 type = INODE_DIR;
    const char *p = path;
    while (*p == '/') p++;

    char name[RVFS_NAME_LEN + 1];
    while (*p) {
        u32 n = 0;
        while (*p && *p != '/') {
            if (n > RVFS_NAME_LEN) return RVFS_EINVAL;
            name[n++] = *p++;
        }
        name[n] = 0;
        while (*p == '/') p++;
        if (n == 0)
            continue;
        if (type != INODE_DIR)
            return RVFS_ENOTDIR;
        int r = dir_lookup_ino(cur, name, &cur, &type);
        if (r) return r;
    }
    *out_ino = cur;
    *out_type = type;
    return RVFS_OK;
}

/* ------------------------------------------------------------------ */
/*  Mount / format                                                    */
/* ------------------------------------------------------------------ */
static int super_valid(const RvSuper *s)
{
    if (s->magic != RVFS_MAGIC || s->version != RVFS_VERSION)
        return 0;
    if (s->block_size != RVFS_BLOCK)
        return 0;
    if (s->total_blocks == 0 || s->total_blocks > RVFS_MAX_BLOCKS)
        return 0;
    if (s->block_bitmap_blocks == 0 ||
        s->block_bitmap_blocks > RVFS_MAX_BITMAP)
        return 0;
    if (s->data_start >= s->total_blocks)
        return 0;
    if (s->inode_count == 0 || s->inode_count > 4096)
        return 0;
    if (s->root_ino == 0 || s->root_ino > s->inode_count)
        return 0;
    if (s->block_bitmap_block + s->block_bitmap_blocks > s->total_blocks)
        return 0;
    if (s->inode_bitmap_block + s->inode_bitmap_blocks > s->total_blocks)
        return 0;
    return 1;
}

int rvfs_mount(const DiskPart *part)
{
    if (!part)
        return RVFS_EINVAL;

    vol_part = *part;
    memset(&sb, 0, sizeof(sb));
    /* Read block 0 directly: rvblk_read() validates against sb.total_blocks. */
    if (disk_read_part(&vol_part, 0, RVFS_SECTORS, blk_buf) != 0)
        return RVFS_EIO;
    memcpy(&sb, blk_buf, sizeof(sb));
    if (!super_valid(&sb))
        return RVFS_EINVAL;

    for (u32 i = 0; i < sb.block_bitmap_blocks; i++) {
        int r = rvblk_read(sb.block_bitmap_block + i,
                         block_bitmap + i * RVFS_BLOCK);
        if (r) return r;
    }
    for (u32 i = 0; i < sb.inode_bitmap_blocks; i++) {
        int r = rvblk_read(sb.inode_bitmap_block + i,
                         inode_bitmap + i * RVFS_BLOCK);
        if (r) return r;
    }

    sb.label[31] = 0;
    memcpy(vol_info.label, sb.label, 32);
    vol_info.block_size = sb.block_size;
    vol_info.total_blocks = sb.total_blocks;
    vol_info.inode_count = sb.inode_count;
    vol_info.free_blocks = 0;
    for (u32 b = sb.data_start; b < sb.total_blocks; b++)
        if (!bit_get(block_bitmap, b))
            vol_info.free_blocks++;
    vol_info.free_inodes = 0;
    for (u32 i = 1; i < sb.inode_count; i++)
        if (!bit_get(inode_bitmap, i))
            vol_info.free_inodes++;

    mounted = 1;
    dirty_sb = 0;
    return RVFS_OK;
}

void rvfs_unmount(void)
{
    mounted = 0;
}

int rvfs_mounted(void) { return mounted; }

int rvfs_mounted_on(const DiskPart *part)
{
    return mounted && part && part->dev == vol_part.dev &&
           part->start_lba == vol_part.start_lba;
}

const RvfsVolume *rvfs_volume(void) { return mounted ? &vol_info : 0; }

int rvfs_sync(void)
{
    if (!mounted)
        return RVFS_EIO;
    int rc = map_flush();
    int r = sb_flush();
    if (!rc) rc = r;
    if (vol_part.dev >= 0)
        blk_flush(vol_part.dev);
    return rc;
}

static int init_dir_inode(u32 ino, u32 parent)
{
    RvInode in;
    memset(&in, 0, sizeof(in));
    in.mode = INODE_DIR;
    in.links = 2;
    in.size = 0;
    int r = inode_write(ino, &in);
    if (r) return r;
    r = dir_add(ino, ".", ino, INODE_DIR);
    if (r) return r;
    return dir_add(ino, "..", parent, INODE_DIR);
}

int rvfs_format(const DiskPart *part, const char *label)
{
    if (!part)
        return RVFS_EINVAL;

    u32 total_blocks = part->sectors / RVFS_SECTORS;
    if (total_blocks > RVFS_MAX_BLOCKS)
        total_blocks = RVFS_MAX_BLOCKS;
    if (total_blocks < 32)
        return RVFS_EINVAL;

    RvSuper s;
    memset(&s, 0, sizeof(s));
    s.magic = RVFS_MAGIC;
    s.version = RVFS_VERSION;
    s.block_size = RVFS_BLOCK;
    s.total_blocks = total_blocks;
    s.inode_count = 1024;
    s.inode_blocks = (s.inode_count + RVFS_INODES_BLK - 1) / RVFS_INODES_BLK;
    s.block_bitmap_blocks = (total_blocks + RVFS_BLOCK * 8 - 1) /
                            (RVFS_BLOCK * 8);
    s.block_bitmap_block = 1 + s.inode_blocks;
    s.inode_bitmap_block = s.block_bitmap_block + s.block_bitmap_blocks;
    s.inode_bitmap_blocks = 1;
    s.data_start = s.inode_bitmap_block + s.inode_bitmap_blocks;
    s.root_ino = 1;
    s.free_blocks = total_blocks - s.data_start;
    s.free_inodes = s.inode_count - 1;
    if (label) {
        u32 n = strlen(label);
        if (n > 31) n = 31;
        memcpy(s.label, label, n);
    }

    /* Start from a clean, mounted-looking state so the helpers work. */
    vol_part = *part;
    sb = s;
    mounted = 1;

    memset(block_bitmap, 0, sizeof(block_bitmap));
    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    for (u32 b = 0; b < s.data_start; b++)
        bit_set(block_bitmap, b, 1);
    bit_set(inode_bitmap, 1, 1);

    int rc = map_flush();
    if (rc) goto fail;

    /* Zero the inode table and the first data block. */
    memset(blk_buf, 0, sizeof(blk_buf));
    for (u32 i = 0; i < s.inode_blocks; i++) {
        rc = rvblk_write(1 + i, blk_buf);
        if (rc) goto fail;
    }
    for (u32 i = 0; i < s.block_bitmap_blocks; i++) {
        if (i == 0) continue;
        rc = rvblk_write(s.block_bitmap_block + i, blk_buf);
        if (rc) goto fail;
    }

    rc = init_dir_inode(1, 1);
    if (rc) goto fail;

    dirty_sb = 1;
    rc = sb_flush();
    if (rc) goto fail;
    rc = map_flush();

    if (rc == RVFS_OK) {
        u32 free_inodes = 0;
        for (u32 i = 1; i < s.inode_count; i++)
            if (!bit_get(inode_bitmap, i))
                free_inodes++;
        memcpy(vol_info.label, s.label, 32);
        vol_info.label[31] = 0;
        vol_info.block_size = s.block_size;
        vol_info.total_blocks = s.total_blocks;
        vol_info.inode_count = s.inode_count;
        vol_info.free_blocks = sb.free_blocks;
        vol_info.free_inodes = free_inodes;
        blk_flush(part->dev);
    }
    return rc;

fail:
    mounted = 0;
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Public operations                                                 */
/* ------------------------------------------------------------------ */
int rvfs_stat(const char *path, RvfsStat *out)
{
    u32 ino;
    u8 type;
    int r = resolve(path, &ino, &type);
    if (r) return r;
    RvInode in;
    r = inode_read(ino, &in);
    if (r) return r;
    if (out) {
        out->type = in.mode;
        out->ino = ino;
        out->size = in.size;
        out->links = in.links;
        out->blocks = in.size / RVFS_BLOCK + 1;
    }
    return RVFS_OK;
}

int rvfs_readdir(const char *path, u32 index, RvfsEntry *out)
{
    u32 ino;
    u8 type;
    int r = resolve(path, &ino, &type);
    if (r) return r;
    if (type != INODE_DIR)
        return RVFS_ENOTDIR;

    u32 size;
    r = dir_load(ino, &size);
    if (r) return r;

    u32 pos = 0, seen = 0;
    while (pos + sizeof(RvDirHeader) <= size) {
        RvDirHeader h;
        memcpy(&h, dir_buf + pos, sizeof(h));
        u32 need = dir_entry_size(h.name_len);
        if (h.name_len == 0 || pos + need > size)
            break;
        if (h.ino != 0) {
            if (seen == index) {
                RvInode in;
                r = inode_read(h.ino, &in);
                if (r) return r;
                memset(out, 0, sizeof(*out));
                memcpy(out->name, dir_buf + pos + sizeof(h), h.name_len);
                out->name[h.name_len] = 0;
                out->ino = h.ino;
                out->type = h.type;
                out->size = in.size;
                return 1;
            }
            seen++;
        }
        pos += need;
    }
    return 0;
}

int rvfs_read(const char *path, u32 offset, void *buf, u32 len)
{
    u32 ino;
    u8 type;
    int r = resolve(path, &ino, &type);
    if (r) return r;
    if (type != INODE_FILE)
        return RVFS_EISDIR;
    RvInode in;
    r = inode_read(ino, &in);
    if (r) return r;
    if (offset >= in.size)
        return 0;
    u32 avail = in.size - offset;
    if (len > avail) len = avail;
    r = inode_read_data(&in, offset, buf, len);
    if (r) return r;
    return (int)len;
}

int rvfs_write(const char *path, u32 offset, const void *buf, u32 len)
{
    u32 ino;
    u8 type;
    int r = resolve(path, &ino, &type);
    if (r) return r;
    if (type != INODE_FILE)
        return RVFS_EISDIR;
    if (offset + len > (RVFS_DIRECT + RVFS_PTRS) * RVFS_BLOCK)
        return RVFS_EFBIG;
    RvInode in;
    r = inode_read(ino, &in);
    if (r) return r;
    if (offset > in.size)
        return RVFS_EINVAL;
    r = inode_write_data(ino, &in, offset, buf, len);
    if (r) return r;
    return (int)len;
}

int rvfs_create(const char *path)
{
    char parent[128], name[RVFS_NAME_LEN + 1];
    int r = split_parent(path, parent, name);
    if (r) return r;
    u32 pino;
    u8 ptype;
    r = resolve(parent, &pino, &ptype);
    if (r) return r;
    if (ptype != INODE_DIR)
        return RVFS_ENOTDIR;

    u32 existing;
    u8 etype;
    if (dir_lookup_ino(pino, name, &existing, &etype) == RVFS_OK) {
        if (etype != INODE_FILE)
            return RVFS_EISDIR;
        RvInode in;
        r = inode_read(existing, &in);
        if (r) return r;
        free_inode_data(&in);
        in.size = 0;
        return inode_write(existing, &in);
    }

    u32 ino;
    r = alloc_inode(&ino);
    if (r) return r;
    RvInode in;
    memset(&in, 0, sizeof(in));
    in.mode = INODE_FILE;
    in.links = 1;
    r = inode_write(ino, &in);
    if (r) return r;
    r = dir_add(pino, name, ino, INODE_FILE);
    if (r) {
        free_inode(ino);
        return r;
    }
    return ino;
}

int rvfs_mkdir(const char *path)
{
    char parent[128], name[RVFS_NAME_LEN + 1];
    int r = split_parent(path, parent, name);
    if (r) return r;
    u32 pino;
    u8 ptype;
    r = resolve(parent, &pino, &ptype);
    if (r) return r;
    if (ptype != INODE_DIR)
        return RVFS_ENOTDIR;

    u32 existing;
    u8 etype;
    if (dir_lookup_ino(pino, name, &existing, &etype) == RVFS_OK)
        return RVFS_EEXIST;

    u32 ino;
    r = alloc_inode(&ino);
    if (r) return r;
    r = init_dir_inode(ino, pino);
    if (r) {
        free_inode(ino);
        return r;
    }
    r = dir_add(pino, name, ino, INODE_DIR);
    if (r) {
        free_inode(ino);
        return r;
    }
    return ino;
}

int rvfs_unlink(const char *path)
{
    char parent[128], name[RVFS_NAME_LEN + 1];
    int r = split_parent(path, parent, name);
    if (r) return r;
    u32 pino;
    u8 ptype;
    r = resolve(parent, &pino, &ptype);
    if (r) return r;

    u32 ino;
    u8 type;
    r = dir_lookup_ino(pino, name, &ino, &type);
    if (r) return r;

    RvInode in;
    r = inode_read(ino, &in);
    if (r) return r;

    if (in.mode == INODE_DIR) {
        u32 size;
        r = dir_load(ino, &size);
        if (r) return r;
        u32 pos = 0;
        while (pos + sizeof(RvDirHeader) <= size) {
            RvDirHeader h;
            memcpy(&h, dir_buf + pos, sizeof(h));
            u32 need = dir_entry_size(h.name_len);
            if (h.name_len == 0 || pos + need > size)
                break;
            if (h.ino && h.name_len != 1 && h.name_len != 2)
                return RVFS_ENOTEMPTY;
            pos += need;
        }
    }

    r = dir_remove(pino, name);
    if (r) return r;
    free_inode_data(&in);
    free_inode(ino);
    rvfs_sync();
    return RVFS_OK;
}
