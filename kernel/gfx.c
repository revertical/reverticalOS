#include "gfx.h"
#include "serial.h"
#include "string.h"
#include "trig.h"

#define MAX_W 1600
#define MAX_H 1200

static u32 buffer_back[MAX_W * MAX_H];
static u32 buffer_wall[MAX_W * MAX_H];
static u32 buffer_boot[MAX_W * MAX_H];

Gfx gfx;

static int clip_x0, clip_y0, clip_x1, clip_y1;

/* ------------------------------------------------------------------------- */
/*  Low level colour helpers                                                  */
/* ------------------------------------------------------------------------- */
static inline u32 blend_rgb(u32 dst, u32 src, u32 a)
{
    if (a == 0) return dst;
    if (a >= 255) return src & 0xFFFFFFu;
    u32 inv = 255 - a;
    u32 rb = (((dst & 0xFF00FFu) * inv + (src & 0xFF00FFu) * a) >> 8) & 0xFF00FFu;
    u32 g  = ((((dst >> 8) & 0xFFu) * inv + ((src >> 8) & 0xFFu) * a) >> 8) & 0xFFu;
    return rb | (g << 8);
}

static inline void put_pixel(u32 *p, u32 color, u32 alpha)
{
    *p = blend_rgb(*p, color, alpha);
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline u32 clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (u32)v); }

static inline void plot(int x, int y, u32 color, u32 alpha)
{
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    if (!alpha) return;
    put_pixel(&gfx.back[(u32)y * gfx.width + (u32)x], color, alpha);
}

void gfx_set_clip(int x, int y, int w, int h)
{
    clip_x0 = imax(0, x);
    clip_y0 = imax(0, y);
    clip_x1 = imin((int)gfx.width, x + w);
    clip_y1 = imin((int)gfx.height, y + h);
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}

void gfx_reset_clip(void)
{
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = (int)gfx.width;
    clip_y1 = (int)gfx.height;
}

/* ------------------------------------------------------------------------- */
/*  Init / buffers                                                            */
/* ------------------------------------------------------------------------- */
void gfx_init(const BootInfo *bi)
{
    gfx.width = bi->width;
    gfx.height = bi->height;
    gfx.stride = bi->pitch / 4;
    gfx.fb = (u32 *)(uintptr_t)bi->fb_phys;
    gfx_reset_clip();
    gfx.back = buffer_back;
    gfx.wallpaper = buffer_wall;
    gfx.bootbg = buffer_boot;

    serial_write("gfx: ");
    serial_dec(gfx.width);
    serial_write("x");
    serial_dec(gfx.height);
    serial_write(" stride ");
    serial_dec(gfx.stride);
    serial_write("\r\n");

    if (gfx.width > MAX_W || gfx.height > MAX_H) {
        serial_write("gfx: resolution too large\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

static void scale_cover(const RvImage *src, u32 *dst, int dw, int dh)
{
    int sw = (int)src->width;
    int sh = (int)src->height;
    int src_w, src_h, sx0, sy0;

    if (sw * dh > sh * dw) {
        src_h = sh;
        src_w = (sh * dw) / dh;
        sx0 = (sw - src_w) / 2;
        sy0 = 0;
    } else {
        src_w = sw;
        src_h = (sw * dh) / dw;
        sx0 = 0;
        sy0 = (sh - src_h) / 2;
    }
    if (src_w < 1) src_w = 1;
    if (src_h < 1) src_h = 1;

    int stepx = (src_w << 16) / dw;
    int stepy = (src_h << 16) / dh;
    int fx = sx0 << 16;
    int fy = sy0 << 16;

    for (int y = 0; y < dh; y++) {
        int sy = fy >> 16;
        int wy = (fy >> 8) & 0xFF;
        int sy1 = sy + 1;
        if (sy1 >= sh) sy1 = sh - 1;
        u32 *row0 = src->pixels + (u32)sy * sw;
        u32 *row1 = src->pixels + (u32)sy1 * sw;
        u32 *out = dst + (u32)y * gfx.width;
        int xf = fx;
        for (int x = 0; x < dw; x++) {
            int sx = xf >> 16;
            int wx = (xf >> 8) & 0xFF;
            int sx1 = sx + 1;
            if (sx1 >= sw) sx1 = sw - 1;
            u32 c00 = row0[sx], c10 = row0[sx1];
            u32 c01 = row1[sx], c11 = row1[sx1];
            u32 a = blend_rgb(c00, c10, (u32)wx);
            u32 b = blend_rgb(c01, c11, (u32)wx);
            out[x] = blend_rgb(a, b, (u32)wy) | 0xFF000000u;
            xf += stepx;
        }
        fy += stepy;
    }
}

void gfx_load_backgrounds(void)
{
    RvImage img;
    if (!asset_load(ASSET_WALLPAPER, &img)) {
        serial_write("gfx: wallpaper missing\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    scale_cover(&img, gfx.wallpaper, (int)gfx.width, (int)gfx.height);

    if (asset_load(ASSET_BOOTBG, &img))
        scale_cover(&img, gfx.bootbg, (int)gfx.width, (int)gfx.height);
    else
        memset(gfx.bootbg, 0, gfx.width * gfx.height * 4);

    serial_write("gfx: backgrounds ready\r\n");
}

void gfx_present(void)
{
    for (u32 y = 0; y < gfx.height; y++)
        memcpy(gfx.fb + (u32)y * gfx.stride, gfx.back + (u32)y * gfx.width,
               gfx.width * 4);
}

void gfx_present_fade(const u32 *from, u8 t)
{
    for (u32 y = 0; y < gfx.height; y++) {
        const u32 *src = from + y * gfx.width;
        u32 *dst = gfx.fb + y * gfx.stride;
        const u32 *cur = gfx.back + y * gfx.width;
        for (u32 x = 0; x < gfx.width; x++)
            dst[x] = blend_rgb(src[x], cur[x], t);
    }
}

void gfx_reload_wallpaper(void)
{
    RvImage img;
    if (asset_load(ASSET_WALLPAPER, &img))
        scale_cover(&img, gfx.wallpaper, (int)gfx.width, (int)gfx.height);
}

void gfx_copy_wallpaper(void)
{
    memcpy(gfx.back, gfx.wallpaper, gfx.width * gfx.height * 4);
}

void gfx_clear(u32 color)
{
    for (u32 i = 0; i < gfx.width * gfx.height; i++)
        gfx.back[i] = color;
}

/* ------------------------------------------------------------------------- */
/*  Rectangles                                                                */
/* ------------------------------------------------------------------------- */
void gfx_pixel(int x, int y, u32 color, u8 alpha)
{
    plot(x, y, color, alpha);
}

void gfx_fill_rect(int x, int y, int w, int h, u32 color)
{
    gfx_fill_rect_a(x, y, w, h, color, 255);
}

void gfx_fill_rect_a(int x, int y, int w, int h, u32 color, u8 alpha)
{
    int x0 = imax(x, clip_x0);
    int y0 = imax(y, clip_y0);
    int x1 = imin(x + w, clip_x1);
    int y1 = imin(y + h, clip_y1);
    if (x0 >= x1 || y0 >= y1 || alpha == 0) return;
    u32 c = color & 0xFFFFFFu;
    if (alpha == 255) {
        for (int yy = y0; yy < y1; yy++) {
            u32 *row = gfx.back + (u32)yy * gfx.width;
            for (int xx = x0; xx < x1; xx++) row[xx] = c;
        }
    } else {
        for (int yy = y0; yy < y1; yy++) {
            u32 *row = gfx.back + (u32)yy * gfx.width;
            for (int xx = x0; xx < x1; xx++) put_pixel(&row[xx], c, alpha);
        }
    }
}

static u8 corner_coverage(int px, int py, int r)
{
    int inside = 0;
    int rr = r * 4;
    int limit = rr * rr;
    for (int sy = 0; sy < 4; sy++) {
        int dy = py * 4 + sy * 2 + 1 - rr;
        for (int sx = 0; sx < 4; sx++) {
            int dx = px * 4 + sx * 2 + 1 - rr;
            if (dx * dx + dy * dy <= limit)
                inside++;
        }
    }
    return (u8)(inside * 255 / 16);
}

void gfx_fill_round(int x, int y, int w, int h, int radius, u32 color, u8 alpha)
{
    if (w <= 0 || h <= 0) return;
    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        gfx_fill_rect_a(x, y, w, h, color, alpha);
        return;
    }

    gfx_fill_rect_a(x + r, y, w - 2 * r, h, color, alpha);
    gfx_fill_rect_a(x, y + r, r, h - 2 * r, color, alpha);
    gfx_fill_rect_a(x + w - r, y + r, r, h - 2 * r, color, alpha);

    for (int py = 0; py < r; py++) {
        for (int px = 0; px < r; px++) {
            u8 cov = corner_coverage(px, py, r);
            if (!cov) continue;
            u32 a = (u32)alpha * cov / 255;
            if (!a) continue;
            plot(x + px, y + py, color, a);
            plot(x + w - 1 - px, y + py, color, a);
            plot(x + px, y + h - 1 - py, color, a);
            plot(x + w - 1 - px, y + h - 1 - py, color, a);
        }
    }
}

void gfx_panel(int x, int y, int w, int h, int radius, u32 fill, u8 fill_alpha,
               u32 border, u8 border_alpha)
{
    if (border_alpha) {
        gfx_fill_round(x, y, w, h, radius, border, border_alpha);
        gfx_fill_round(x + 1, y + 1, w - 2, h - 2, radius - 1, fill, fill_alpha);
    } else {
        gfx_fill_round(x, y, w, h, radius, fill, fill_alpha);
    }
}

void gfx_shadow_round(int x, int y, int w, int h, int radius, int spread,
                      u32 color, u8 max_alpha)
{
    int layers = 5;
    for (int i = layers; i >= 1; i--) {
        int e = spread * i / layers;
        u8 a = (u8)((u32)max_alpha * (u32)(layers + 1 - i) / ((u32)layers * 3));
        gfx_fill_round(x - e, y - e + 4, w + e * 2, h + e * 2, radius + e, color, a);
    }
}

/* ------------------------------------------------------------------------- */
/*  Circles and arcs                                                          */
/* ------------------------------------------------------------------------- */
static u32 isqrt32(u32 v)
{
    u32 r = 0;
    u32 bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

void gfx_fill_circle(int cx, int cy, int radius, u32 color, u8 alpha)
{
    if (radius <= 0 || alpha == 0) return;
    int r = radius;
    u32 rr = (u32)(r * r);
    for (int dy = -r; dy <= r; dy++) {
        u32 rem = rr - (u32)(dy * dy);
        int wx = (int)isqrt32(rem);
        /* soft edge: the pixel just outside is given partial coverage */
        u32 edge = rem > (u32)(wx * wx + 2 * wx) ? alpha / 4 : 0;
        int x0 = cx - wx;
        int x1 = cx + wx;
        if (x0 < clip_x0) x0 = clip_x0;
        if (x1 >= clip_x1) x1 = clip_x1 - 1;
        int yy = cy + dy;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        u32 *row = gfx.back + (u32)yy * gfx.width;
        for (int x = x0; x <= x1; x++) put_pixel(&row[x], color, alpha);
        if (edge) {
            int ex = cx + wx + 1;
            if (ex >= clip_x0 && ex < clip_x1) put_pixel(&row[ex], color, (u8)edge);
            ex = cx - wx - 1;
            if (ex >= clip_x0 && ex < clip_x1) put_pixel(&row[ex], color, (u8)edge);
        }
    }
}

void gfx_radial(int cx, int cy, int radius, u32 inner, u32 outer, u8 alpha)
{
    if (radius <= 0 || alpha == 0) return;
    int r = radius;
    int x0 = imax(clip_x0, cx - r), x1 = imin(clip_x1, cx + r + 1);
    int y0 = imax(clip_y0, cy - r), y1 = imin(clip_y1, cy + r + 1);
    u32 rr = (u32)(r * r);
    for (int y = y0; y < y1; y++) {
        int dy = y - cy;
        u32 *row = gfx.back + (u32)y * gfx.width;
        for (int x = x0; x < x1; x++) {
            int dx = x - cx;
            u32 d2 = (u32)(dx * dx + dy * dy);
            if (d2 > rr) continue;
            u32 d = isqrt32(d2);
            u32 t = d * 255u / (u32)r;
            u32 fall = 255u - t;
            u32 a = (u32)alpha * fall * fall / 255u / 255u;
            if (!a) continue;
            u32 c = blend_rgb(inner & 0xFFFFFF, outer & 0xFFFFFF, t);
            put_pixel(&row[x], c, a);
        }
    }
}

void gfx_stamp_disc(int cx, int cy, int radius, u32 color, u8 alpha)
{
    gfx_fill_circle(cx, cy, radius, color, alpha);
}

void gfx_arc(int cx, int cy, int radius, int thickness, int start_deg, int end_deg,
             u32 color, u8 start_alpha, u8 end_alpha)
{
    if (end_deg <= start_deg) return;
    int steps = (end_deg - start_deg) * 2;
    if (steps < 2) steps = 2;
    int rad = thickness / 2;
    if (rad < 1) rad = 1;
    for (int i = 0; i <= steps; i++) {
        int t = i * 255 / steps;
        int ang = (start_deg + (end_deg - start_deg) * i / steps) % 360;
        int idx = ang * 256 / 360;
        int x = cx + (radius * cos_i(idx)) / 65536;
        int y = cy + (radius * sin_i(idx)) / 65536;
        int a = (int)start_alpha + ((int)end_alpha - (int)start_alpha) * t / 255;
        gfx_stamp_disc(x, y, rad, color, (u8)(a < 0 ? 0 : (a > 255 ? 255 : a)));
    }
}

void gfx_ellipse_dots(int cx, int cy, int rx, int ry, u32 color, u8 alpha, int phase)
{
    for (int d = 0; d < 360; d += 7) {
        int idx = ((d + phase) % 360) * 256 / 360;
        int x = cx + rx * cos_i(idx) / 65536;
        int y = cy + ry * sin_i(idx) / 65536;
        gfx_fill_rect(x, y, 2, 2, color);
    }
}

/* ------------------------------------------------------------------------- */
/*  Sprites                                                                   */
/* ------------------------------------------------------------------------- */
void gfx_blit(const RvImage *img, int x, int y)
{
    gfx_blit_alpha(img, x, y, 255);
}

void gfx_blit_alpha(const RvImage *img, int x, int y, u8 alpha)
{
    if (!img || !img->pixels) return;
    int x0 = imax(clip_x0, x);
    int y0 = imax(clip_y0, y);
    int x1 = imin(clip_x1, x + (int)img->width);
    int y1 = imin(clip_y1, y + (int)img->height);
    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = y0; yy < y1; yy++) {
        const u32 *src = img->pixels + (u32)(yy - y) * img->width;
        u32 *dst = gfx.back + (u32)yy * gfx.width;
        for (int xx = x0; xx < x1; xx++) {
            u32 p = src[xx - x];
            u32 a = (p >> 24) * alpha / 255;
            if (!a) continue;
            if (a >= 255) dst[xx] = p & 0xFFFFFFu;
            else put_pixel(&dst[xx], p & 0xFFFFFFu, a);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Text                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct {
    const FontDef *def;
    RvImage atlas;
    int loaded;
} FontCache;

static FontCache fonts[8];
static int font_count;

static FontCache *font_cache(const FontDef *def)
{
    for (int i = 0; i < font_count; i++)
        if (fonts[i].def == def) return &fonts[i];
    if (font_count >= 8) return 0;
    FontCache *fc = &fonts[font_count++];
    fc->def = def;
    fc->loaded = asset_load(def->asset, &fc->atlas);
    if (!fc->loaded) {
        serial_write("gfx: missing font atlas\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    return fc;
}

void gfx_fonts_init(void)
{
    font_cache(&g_font_ui13);
    font_cache(&g_font_ui15);
    font_cache(&g_font_ui15sb);
    font_cache(&g_font_title19);
}

static const GlyphMetric *find_glyph(const FontDef *font, u16 cp)
{
    for (u16 i = 0; i < font->count; i++) {
        if (font->glyphs[i].cp == cp)
            return &font->glyphs[i];
    }
    return 0;
}

int gfx_text_width(const FontDef *font, const char *text, int tracking)
{
    int w = 0;
    for (const char *p = text; *p; p++) {
        const GlyphMetric *g = find_glyph(font, (u8)*p);
        if (!g) continue;
        w += g->adv + tracking;
    }
    if (w > 0) w -= tracking;
    return w;
}

int gfx_text(const FontDef *font, int x, int baseline, const char *text,
             u32 color, u8 alpha, int tracking)
{
    FontCache *fc = font_cache(font);
    if (!fc) return x;
    int aw = (int)fc->atlas.width;
    int pen = x;
    u32 c = color & 0xFFFFFFu;

    for (const char *p = text; *p; p++) {
        const GlyphMetric *g = find_glyph(font, (u8)*p);
        if (!g) continue;
        if (g->w && g->h) {
            int gx = pen + g->ox;
            int gy = baseline + g->oy;
            int x0 = imax(clip_x0, gx);
            int y0 = imax(clip_y0, gy);
            int x1 = imin(clip_x1, gx + g->w);
            int y1 = imin(clip_y1, gy + g->h);
            for (int yy = y0; yy < y1; yy++) {
                const u32 *srow = fc->atlas.pixels + (u32)(g->y + (yy - gy)) * aw;
                u32 *drow = gfx.back + (u32)yy * gfx.width;
                for (int xx = x0; xx < x1; xx++) {
                    u32 cov = (srow[g->x + (xx - gx)] >> 16) & 0xFFu;
                    if (!cov) continue;
                    u32 a = cov * alpha / 255;
                    put_pixel(&drow[xx], c, a);
                }
            }
        }
        pen += g->adv + tracking;
    }
    return pen;
}

/* ------------------------------------------------------------------------- */
/*  Cross-fade                                                                */
/* ------------------------------------------------------------------------- */
void gfx_fade_buffers(const u32 *a, const u32 *b, u32 *dst, u32 count, u8 t)
{
    for (u32 i = 0; i < count; i++) {
        dst[i] = blend_rgb(a[i], b[i], t) | 0xFF000000u;
    }
}
