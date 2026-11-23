#ifndef RVOS_GFX_H
#define RVOS_GFX_H

#include "types.h"
#include "bootinfo.h"
#include "assets.h"

typedef struct {
    u32 width;
    u32 height;
    u32 stride;      /* pixels per scanline */
    u32 *fb;         /* hardware framebuffer */
    u32 *back;       /* composition buffer */
    u32 *wallpaper;  /* desktop wallpaper, scaled to the screen */
    u32 *bootbg;     /* boot animation background */
} Gfx;

extern Gfx gfx;

void gfx_init(const BootInfo *bi);
void gfx_load_backgrounds(void);
void gfx_fonts_init(void);

void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);
void gfx_present(void);
void gfx_present_fade(const u32 *from, u8 t);
void gfx_reload_wallpaper(void);
void gfx_copy_wallpaper(void);
void gfx_clear(u32 color);

void gfx_pixel(int x, int y, u32 color, u8 alpha);
void gfx_fill_rect(int x, int y, int w, int h, u32 color);
void gfx_fill_rect_a(int x, int y, int w, int h, u32 color, u8 alpha);
void gfx_fill_round(int x, int y, int w, int h, int radius, u32 color, u8 alpha);
void gfx_panel(int x, int y, int w, int h, int radius, u32 fill, u8 fill_alpha,
               u32 border, u8 border_alpha);
void gfx_shadow_round(int x, int y, int w, int h, int radius, int spread,
                      u32 color, u8 max_alpha);
void gfx_fill_circle(int cx, int cy, int radius, u32 color, u8 alpha);
void gfx_radial(int cx, int cy, int radius, u32 inner, u32 outer, u8 alpha);
void gfx_stamp_disc(int cx, int cy, int radius, u32 color, u8 alpha);
void gfx_arc(int cx, int cy, int radius, int thickness, int start_deg, int end_deg,
             u32 color, u8 start_alpha, u8 end_alpha);
void gfx_ellipse_dots(int cx, int cy, int rx, int ry, u32 color, u8 alpha, int phase);

void gfx_blit(const RvImage *img, int x, int y);
void gfx_blit_alpha(const RvImage *img, int x, int y, u8 alpha);

int  gfx_text(const FontDef *font, int x, int baseline, const char *text,
              u32 color, u8 alpha, int tracking);
int  gfx_text_width(const FontDef *font, const char *text, int tracking);

void gfx_fade_buffers(const u32 *a, const u32 *b, u32 *dst, u32 count, u8 t);

#endif
