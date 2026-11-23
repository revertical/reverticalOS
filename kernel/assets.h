#ifndef RVOS_ASSETS_H
#define RVOS_ASSETS_H

#include "types.h"
#include "assets_ids.h"

typedef struct {
    u16 cp;
    u16 x, y, w, h;
    i16 ox, oy;
    u16 adv;
} GlyphMetric;

typedef struct {
    u32 asset;
    u16 ascent;
    u16 line_height;
    u16 count;
    const GlyphMetric *glyphs;
} FontDef;

typedef struct {
    u32 width;
    u32 height;
    u32 *pixels;    /* 0xAARRGGBB */
} RvImage;

extern const FontDef g_font_ui13;
extern const FontDef g_font_ui15;
extern const FontDef g_font_ui15sb;
extern const FontDef g_font_title19;

void  assets_init(void);
int   asset_load(u32 id, RvImage *out);
void *arena_alloc(u32 bytes);
void  arena_reset(void);

#endif
