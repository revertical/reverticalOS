#include "bootanim.h"
#include "gfx.h"
#include "assets.h"
#include "assets_ids.h"
#include "timer.h"
#include "serial.h"
#include "string.h"
#include "trig.h"

#define BOOT_MS 4000

static int ui_scale = 100;

static int B(int v)
{
    return (v * ui_scale + 50) / 100;
}

static int imin(int a, int b) { return a < b ? a : b; }

static void text_at(const FontDef *f, int x, int baseline, const char *s,
                    u32 color, u8 alpha)
{
    gfx_text(f, x, baseline, s, color, alpha, 0);
}

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

static u32 mixc(u32 a, u32 b, int k)
{
    if (k < 0) k = 0;
    if (k > 255) k = 255;
    int inv = 255 - k;
    int r = (((int)((a >> 16) & 0xFF) * inv) + (int)((b >> 16) & 0xFF) * k) / 255;
    int g = (((int)((a >> 8) & 0xFF) * inv) + (int)((b >> 8) & 0xFF) * k) / 255;
    int bl = (((int)(a & 0xFF) * inv) + (int)(b & 0xFF) * k) / 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

/* Cheap 2x2-sampled radial glow (far faster than per-pixel for big radii). */
static void glow_blob(int cx, int cy, int r, u32 color, u8 alpha)
{
    if (r <= 0 || alpha == 0) return;
    u32 rr = (u32)r * (u32)r;
    for (int y = -r; y < r; y += 2) {
        int yy = y + 1;
        for (int x = -r; x < r; x += 2) {
            u32 d2 = (u32)(x * x) + (u32)(yy * yy);
            if (d2 > rr) continue;
            u32 d = isqrt32(d2);
            u32 f = 255u - d * 255u / (u32)r;
            u32 a = (u32)alpha * f * f / 65025u;
            if (a)
                gfx_fill_rect_a(cx + x, cy + y, 2, 2, color, (u8)a);
        }
    }
}

static int boot_percent(u64 t)
{
    if (t < 520) return (int)(t * 12 / 520);
    if (t < 1600) return 12 + (int)((t - 520) * 34 / 1080);
    if (t < 2550) return 46 + (int)((t - 1600) * 27 / 950);
    if (t < 3350) return 73 + (int)((t - 2550) * 20 / 800);
    return 93 + (int)((t - 3350) * 7 / (BOOT_MS - 3350));
}

static const char *boot_stage(u64 t)
{
    if (t < 520) return "Initialising memory";
    if (t < 1600) return "Loading system assets";
    if (t < 2550) return "Starting system services";
    if (t < 3350) return "Compositing desktop";
    return "Ready";
}

static void boot_frame(u64 t, const RvImage *logo, const RvImage *wordmark)
{
    int w = (int)gfx.width;
    int h = (int)gfx.height;
    int cx = w / 2;
    int cy = h * 42 / 100;

    memcpy(gfx.back, gfx.bootbg, (size_t)w * (size_t)h * 4);

    int fade = (int)(t * 255 / 460);
    if (fade > 255) fade = 255;
    if (fade < 0) fade = 0;

    int drift = (int)((t / 34) & 255);
    int dx = 100 * cos_i(drift) / 65536;
    int dy = 50 * sin_i(drift) / 65536;

    glow_blob(cx + B(70) + dx, cy + B(30) + dy, B(330), 0x1B62FF, 42);
    glow_blob(cx - B(170) - dx, cy + B(120) - dy, B(280), 0x8E3BFF, 32);
    glow_blob(cx, cy, B(160), 0x51D8FF, (u8)(36 * fade / 255));

    int ring = B(116);
    int a0 = (int)((t / 9) % 360);
    gfx_arc(cx, cy, ring, B(3), a0, a0 + 112, 0x6FE7FF, 200, 0);
    gfx_arc(cx, cy, ring + B(9), B(2), (a0 + 180) % 360,
            (a0 + 180) % 360 + 78, 0xB58CFF, 150, 0);
    gfx_arc(cx, cy, ring + B(18), B(2), (a0 + 90) % 360,
            (a0 + 90) % 360 + 46, 0xFF7FD0, 110, 0);

    gfx_ellipse_dots(cx, cy, B(158), B(62), 0xCBE7FF, 42, (int)(t / 26));

    static const u32 pcol[4] = { 0x74EAFFu, 0x7E9BFFu, 0xB884FFu, 0xFF8FCFu };
    for (int i = 0; i < 30; i++) {
        int a = (int)(((t / (11 + (u64)(i % 5) * 3)) + (u64)i * 256 / 30) & 255);
        int rx = B(126 + (i % 4) * 9);
        int ry = rx * 40 / 100;
        int px = cx + rx * cos_i(a) / 65536;
        int py = cy + ry * sin_i(a) / 65536;
        u8 alpha = (u8)(70 + ((i * 47) % 100));
        gfx_fill_circle(px, py, B(4), pcol[i % 4], (u8)(alpha / 4));
        gfx_fill_circle(px, py, B(1) + 1, pcol[i % 4], alpha);
    }

    if (logo->pixels)
        gfx_blit_alpha(logo, cx - (int)logo->width / 2,
                       cy - (int)logo->height / 2, (u8)fade);

    int wf = (int)((t > 320 ? t - 320 : 0) * 255 / 520);
    if (wf > 255) wf = 255;
    if (wordmark->pixels && wf > 0)
        gfx_blit_alpha(wordmark, cx - (int)wordmark->width / 2,
                       cy + B(104), (u8)wf);

    if (wf > 0) {
        gfx_text(&g_font_ui13, cx - gfx_text_width(&g_font_ui13,
                 "a desktop, rebuilt from scratch", 0) / 2,
                 cy + B(178), "a desktop, rebuilt from scratch",
                 0x9FC4EA, (u8)(wf * 78 / 255), 0);
    }

    int pct = boot_percent(t);
    int bw = B(384);
    int bh = B(5);
    int bx = cx - bw / 2;
    int by = cy + B(226);

    gfx_fill_round(bx, by, bw, bh, bh / 2, 0xFFFFFF, 34);
    int fw = bw * pct / 100;
    if (fw < bh * 2) fw = bh * 2;
    for (int i = 0; i < fw; i += 2) {
        u32 c = mixc(0x5FE7FF, 0x9A6BFF, i * 255 / bw);
        gfx_fill_rect(bx + i, by, imin(2, fw - i), bh, c);
    }
    gfx_fill_circle(bx + fw, by + bh / 2, B(4), 0x8EDCFF, 60);
    gfx_fill_circle(bx + fw, by + bh / 2, B(2), 0xFFFFFF, 220);

    text_at(&g_font_ui13, bx, by + B(28), boot_stage(t), 0x9FBBDF, 225);

    char pbuf[5];
    int n = 0;
    if (pct >= 100) {
        pbuf[0] = '1'; pbuf[1] = '0'; pbuf[2] = '0'; pbuf[3] = '%';
        n = 4;
    } else if (pct >= 10) {
        pbuf[0] = (char)('0' + pct / 10);
        pbuf[1] = (char)('0' + pct % 10);
        pbuf[2] = '%';
        n = 3;
    } else {
        pbuf[0] = (char)('0' + pct);
        pbuf[1] = '%';
        n = 2;
    }
    pbuf[n] = 0;
    gfx_text(&g_font_ui13, bx + bw - gfx_text_width(&g_font_ui13, pbuf, 0),
             by + B(28), pbuf, 0xDCEBFF, 230, 0);

    const char *foot = "REVERTICAL SYSTEMS   ·   BOOT 2026.11";
    gfx_text(&g_font_ui13, cx - gfx_text_width(&g_font_ui13, foot, 1) / 2,
             h - B(34), foot, 0x7FA4CC, 90, 1);
}

void boot_anim_run(void)
{
    RvImage logo, wordmark;
    if (!asset_load(ASSET_LOGO_LG, &logo))
        memset(&logo, 0, sizeof(logo));
    if (!asset_load(ASSET_WORDMARK, &wordmark))
        memset(&wordmark, 0, sizeof(wordmark));

    ui_scale = (int)gfx.height * 100 / 1024;
    if (ui_scale < 80) ui_scale = 80;
    if (ui_scale > 120) ui_scale = 120;

    u32 *saved_back = gfx.back;
    gfx.back = gfx.wallpaper;         /* boot frames land in the spare buffer */
    gfx_reset_clip();

    serial_write("boot: animation start\r\n");
    u64 t0 = timer_ms();
    for (;;) {
        u64 t = timer_ms() - t0;
        if (t >= BOOT_MS) break;
        boot_frame(t, &logo, &wordmark);
        gfx_present();
    }
    boot_frame(BOOT_MS, &logo, &wordmark);
    gfx_present();

    gfx.back = saved_back;
    gfx_reset_clip();
    serial_write("boot: animation complete\r\n");
}
