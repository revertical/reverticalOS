#include "assets.h"
#include "qoi.h"
#include "serial.h"
#include "string.h"

extern u8 _binary_build_assets_bin_start[];
extern u8 _binary_build_assets_bin_end[];

typedef struct {
    u32 id;
    u32 offset;
    u32 size;
} AssetEntry;

static const u8 *blob;
static u32 blob_size;
static u32 asset_count;
static const AssetEntry *entries;

static u8 arena[8 * 1024 * 1024];
static u32 arena_used;

void arena_reset(void)
{
    arena_used = 0;
}

void *arena_alloc(u32 bytes)
{
    bytes = (bytes + 15) & ~15u;
    if (arena_used + bytes > sizeof(arena)) {
        serial_write("assets: arena exhausted\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    void *p = &arena[arena_used];
    arena_used += bytes;
    return p;
}

void assets_init(void)
{
    blob = _binary_build_assets_bin_start;
    blob_size = (u32)(_binary_build_assets_bin_end - _binary_build_assets_bin_start);
    if (blob_size < 8 || blob[0] != 'R' || blob[1] != 'V' ||
        blob[2] != 'A' || blob[3] != 'S') {
        serial_write("assets: bad container\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    asset_count = *(const u32 *)(blob + 4);
    entries = (const AssetEntry *)(blob + 8);
    serial_write("assets: container with ");
    serial_dec(asset_count);
    serial_write(" entries, ");
    serial_dec(blob_size);
    serial_write(" bytes\r\n");
}

static const AssetEntry *find_asset(u32 id)
{
    for (u32 i = 0; i < asset_count; i++) {
        if (entries[i].id == id)
            return &entries[i];
    }
    return 0;
}

int asset_load(u32 id, RvImage *out)
{
    const AssetEntry *e = find_asset(id);
    if (!e || e->offset + e->size > blob_size)
        return 0;

    const u8 *data = blob + e->offset;
    if (e->size < 14 ||
        data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f')
        return 0;

    u32 w = ((u32)data[4] << 24) | ((u32)data[5] << 16) | ((u32)data[6] << 8) | data[7];
    u32 h = ((u32)data[8] << 24) | ((u32)data[9] << 16) | ((u32)data[10] << 8) | data[11];
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return 0;

    u32 *pixels = (u32 *)arena_alloc(w * h * 4);
    if (!qoi_decode(data, e->size, pixels, w * h, &w, &h))
        return 0;

    out->width = w;
    out->height = h;
    out->pixels = pixels;
    return 1;
}
