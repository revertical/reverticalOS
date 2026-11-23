/* ReverticalOS desktop shell
 *
 * Everything here is drawn with the software compositor in gfx.c: frosted
 * glass panels, the bottom dock, a live clock widget and a small window
 * manager with five first party apps.  The AI app is a designed shell only;
 * its model connection is wired up separately.
 */

#include "ui.h"
#include "gfx.h"
#include "assets.h"
#include "assets_ids.h"
#include "mouse.h"
#include "usb.h"
#include "rtc.h"
#include "timer.h"
#include "serial.h"
#include "string.h"
#include "trig.h"
#include "rvfs.h"
#include "installer.h"
#include "disk.h"
#include "ata.h"
#include "kbd.h"
#include "net.h"
#include "wifi.h"
#include "rvmod.h"
#include "accounts.h"
#include "power.h"

#define APP_COUNT 7
#define DOCK_APPS 5
enum {
    APP_BROWSER = 0,
    APP_FILES,
    APP_MESSAGES,
    APP_SETTINGS,
    APP_AI,
    APP_INSTALLER,
    APP_CONSOLE
};

#define C_INK       0x16213Bu
#define C_INK_SOFT  0x5A6685u
#define C_MUTED     0x8792ACu
#define C_ACCENT    0x3C74FFu
#define C_TEAL      0x16C6C8u
#define C_PURPLE    0x8A5CFFu
#define C_PINK      0xE8469Fu
#define C_GREEN     0x2FBF71u
#define C_RED       0xE5484Du
#define C_PANEL     0xF3F7FEu
#define C_LINE      0xDCE4F2u

typedef struct {
    int x, y, w, h;
    int open;
    int minimized;
    u64 open_time;
} Window;

typedef struct {
    int pill_x, pill_y, pill_w, pill_h;
    int logo_x, logo_y, logo_size;
    int text_x, text_baseline;
    int nav_x[4], nav_y, nav_size, nav_step;
    int dock_x, dock_y, dock_w, dock_h;
    int dock_ix[APP_COUNT], dock_iy, dock_size;
    int card_x, card_y, card_w, card_h;
    int top_x, top_y, top_w, top_h;
    int bot_x, bot_y, bot_w, bot_h;
} Layout;

static Layout L;
static Window wins[APP_COUNT];
static int zorder[APP_COUNT];

static RvImage a_logo_sm, a_logo_lg, a_wordmark;
static RvImage a_dock[APP_COUNT];
static RvImage a_nav[4];
static RvImage a_cursor, a_wifi, a_bars;

static int ui_scale = 100;
static int screen_w, screen_h;

static int nav_hover = -1;
static int dock_hover = -1;
static int close_hover = -1;
static int min_hover = -1;
static int notif_open;
static int panels_hidden;
static int drag_app = -1;
static int drag_dx, drag_dy;

static int live_env = 1;
static int shortcut_hover;
static int inst_wipe_confirm;
static int inst_last_phase = INSTALL_IDLE;

#define NOTIF_MAX 8

typedef struct {
    char text[76];
    u32 color;
    u64 time;
    int read;
} Notif;

enum {
    SCREEN_DESKTOP = 0,
    SCREEN_LOGIN,
    SCREEN_LOCK,
    SCREEN_POWER,
    SCREEN_POWER_SAFE
};

#define START_ACTIONS 3
#define START_ENTRIES (APP_COUNT + START_ACTIONS)

static int screen_mode = SCREEN_DESKTOP;
static int start_open;
static int start_hover = -1;
static int start_sel;
static int power_action;            /* 0 = shut down, 1 = restart */
static u64 power_at;

static int auth_focus;              /* login: 0 user, 1 password */
static char login_user[USER_NAME_MAX + 1];
static char login_pass[USER_PASS_MAX + 1];
static char auth_error[80];

static Notif notifs[NOTIF_MAX];
static int notif_count;
static int notif_hover = -1;
static int seen_eth;
static int seen_wifi;

static int inst_stage;              /* 0 account, 1 disk */
static int inst_focus;
static char inst_user[USER_NAME_MAX + 1];
static char inst_pass[USER_PASS_MAX + 1];
static char inst_confirm[USER_PASS_MAX + 1];
static char inst_error[80];

static void notif_push(const char *text, u32 color);
static int notif_unread(void);
static void start_power(int restart);
static void attempt_login(void);
static void attempt_unlock(void);
static void draw_start_menu(void);

static void path_copy(char *dst, const char *src, int max);
static void text_fit(const FontDef *f, int x, int baseline, const char *s,
                     int maxw, u32 color, u8 alpha);
static void files_refresh(void);
static void files_launch(int index);
static int app_files_click(int x, int y, int w, int h, int mx, int my);
static int app_browser_click(int x, int y, int w, int h, int mx, int my);
static void browser_navigate(const char *url, int w, int record);
static int browser_url_focused(void);
static int app_settings_click(int x, int y, int w, int h, int mx, int my);
static void app_installer_body(int x, int y, int w, int h);
static int app_installer_click(int x, int y, int w, int h, int mx, int my);
static int installed_marker(void);
static void draw_field(int x, int y, int w, int h, int focused,
                       const char *text, const char *placeholder,
                       int password);

static int last_mx = -1, last_my = -1;
static u64 last_rtc_ms;
static u64 last_anim_ms;
static u64 ui_now;

static char clock_text[12] = "10:09 AM";
static char date_text[16] = "14/11/2026";
static int clock_hour = -1, clock_minute = -1, clock_day = -1;

static const char *app_names[APP_COUNT] = {
    "Browser", "Files", "Messages", "Settings", "Revertical AI", "Installer",
    "Program Output"
};

static int S(int v)
{
    return (v * ui_scale + 50) / 100;
}

static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int imax(int a, int b) { return a > b ? a : b; }

static u32 mixc(u32 a, u32 b, int k)
{
    int inv = 255 - k;
    int r = (((int)((a >> 16) & 0xFF) * inv) + (int)((b >> 16) & 0xFF) * k) / 255;
    int g = (((int)((a >> 8) & 0xFF) * inv) + (int)((b >> 8) & 0xFF) * k) / 255;
    int bl = (((int)(a & 0xFF) * inv) + (int)(b & 0xFF) * k) / 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

/* ------------------------------------------------------------------------- */
/*  Text                                                                      */
/* ------------------------------------------------------------------------- */
static int tw(const FontDef *f, const char *s)
{
    return gfx_text_width(f, s, 0);
}

static void text_at(const FontDef *f, int x, int baseline, const char *s,
                    u32 color, u8 alpha)
{
    gfx_text(f, x, baseline, s, color, alpha, 0);
}

static void text_tracked(const FontDef *f, int x, int baseline, const char *s,
                         u32 color, u8 alpha, int tracking)
{
    gfx_text(f, x, baseline, s, color, alpha, tracking);
}

static void text_center(const FontDef *f, int cx, int baseline, const char *s,
                        u32 color, u8 alpha)
{
    gfx_text(f, cx - tw(f, s) / 2, baseline, s, color, alpha, 0);
}

static void line(int x0, int y0, int x1, int y1, u32 color, u8 alpha)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    dx = dx > 0 ? dx : -dx;
    dy = dy > 0 ? dy : -dy;
    if (dx >= dy) {
        int e = -dx;
        for (int i = 0; i <= dx; i++) {
            gfx_pixel(x0, y0, color, alpha);
            e += 2 * dy;
            if (e >= 0) { y0 += sy; e -= 2 * dx; }
            x0 += sx;
        }
    } else {
        int e = -dy;
        for (int i = 0; i <= dy; i++) {
            gfx_pixel(x0, y0, color, alpha);
            e += 2 * dx;
            if (e >= 0) { x0 += sx; e -= 2 * dy; }
            y0 += sy;
        }
    }
}

static void fmt_two(char *out, int v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static void clock_update(void)
{
    RtcTime t;
    rtc_read(&t);
    if (t.hour == clock_hour && t.minute == clock_minute && t.day == clock_day)
        return;
    clock_hour = t.hour;
    clock_minute = t.minute;
    clock_day = t.day;

    int h12 = t.hour % 12;
    if (h12 == 0) h12 = 12;
    clock_text[0] = (char)('0' + h12 / 10);
    clock_text[1] = (char)('0' + h12 % 10);
    clock_text[2] = ':';
    fmt_two(&clock_text[3], t.minute);
    clock_text[5] = ' ';
    clock_text[6] = t.hour >= 12 ? 'P' : 'A';
    clock_text[7] = 'M';
    clock_text[8] = 0;

    fmt_two(&date_text[0], t.day);
    date_text[2] = '/';
    fmt_two(&date_text[3], t.month);
    date_text[5] = '/';
    date_text[6] = '2';
    date_text[7] = '0';
    fmt_two(&date_text[8], t.year % 100);
    date_text[10] = 0;
}

/* ------------------------------------------------------------------------- */
/*  Glass panels                                                              */
/* ------------------------------------------------------------------------- */
static void glass(int x, int y, int w, int h, int r, int shadow)
{
    if (shadow)
        gfx_shadow_round(x, y + S(3), w, h, r, S(15), 0x060D1C, 80);
    gfx_panel(x, y, w, h, r, C_PANEL, 205, 0xFFFFFF, 150);
}

static u32 iridescent(int t)
{
    static const u32 stops[5] = {
        0x59E6D6u, 0x3C74FFu, 0x9A5CFFu, 0xF05FAEu, 0x59E6D6u
    };
    t &= 255;
    int seg = t * 4 / 256;
    int k = (t * 4) % 256;
    return mixc(stops[seg], stops[seg + 1], k);
}

static void rainbow_border(int x, int y, int w, int h, int r)
{
    int steps_edge_x = (w - 2 * r) / 2;
    int steps_edge_y = (h - 2 * r) / 2;
    int corner_steps = 16;
    int total = 2 * steps_edge_x + 2 * steps_edge_y + 4 * corner_steps;
    if (total < 4) total = 4;
    int step = 0;

    /* Top edge, left to right. */
    for (int i = 0; i < steps_edge_x; i++, step++)
        gfx_fill_rect(x + r + i * 2, y, 2, 2, iridescent(step * 255 / total));
    /* Top-right corner. */
    for (int i = 0; i <= corner_steps; i++, step++) {
        int a = (270 + i * 90 / corner_steps) * 256 / 360;
        int px = x + w - 1 - r + r * cos_i(a) / 65536;
        int py = y + r + r * sin_i(a) / 65536;
        gfx_fill_rect(px, py, 2, 2, iridescent(step * 255 / total));
    }
    /* Right edge. */
    for (int i = 0; i < steps_edge_y; i++, step++)
        gfx_fill_rect(x + w - 2, y + r + i * 2, 2, 2,
                      iridescent(step * 255 / total));
    /* Bottom-right corner. */
    for (int i = 0; i <= corner_steps; i++, step++) {
        int a = (i * 90 / corner_steps) * 256 / 360;
        int px = x + w - 1 - r + r * cos_i(a) / 65536;
        int py = y + h - 1 - r + r * sin_i(a) / 65536;
        gfx_fill_rect(px, py, 2, 2, iridescent(step * 255 / total));
    }
    /* Bottom edge, right to left. */
    for (int i = 0; i < steps_edge_x; i++, step++)
        gfx_fill_rect(x + w - r - 2 - i * 2, y + h - 2, 2, 2,
                      iridescent(step * 255 / total));
    /* Bottom-left corner. */
    for (int i = 0; i <= corner_steps; i++, step++) {
        int a = (90 + i * 90 / corner_steps) * 256 / 360;
        int px = x + r + r * cos_i(a) / 65536;
        int py = y + h - 1 - r + r * sin_i(a) / 65536;
        gfx_fill_rect(px, py, 2, 2, iridescent(step * 255 / total));
    }
    /* Left edge. */
    for (int i = 0; i < steps_edge_y; i++, step++)
        gfx_fill_rect(x, y + h - r - 2 - i * 2, 2, 2,
                      iridescent(step * 255 / total));
    /* Top-left corner. */
    for (int i = 0; i <= corner_steps; i++, step++) {
        int a = (180 + i * 90 / corner_steps) * 256 / 360;
        int px = x + r + r * cos_i(a) / 65536;
        int py = y + r + r * sin_i(a) / 65536;
        gfx_fill_rect(px, py, 2, 2, iridescent(step * 255 / total));
    }
}

/* ------------------------------------------------------------------------- */
/*  Layout                                                                    */
/* ------------------------------------------------------------------------- */
static void load_image(RvImage *img, u32 id)
{
    if (!asset_load(id, img)) {
        serial_write("ui: missing asset ");
        serial_dec(id);
        serial_write("\r\n");
        memset(img, 0, sizeof(*img));
    }
}

static void layout_rebuild(void)
{
    screen_w = (int)gfx.width;
    screen_h = (int)gfx.height;
    ui_scale = screen_h * 100 / 1024;
    ui_scale = iclamp(ui_scale, 80, 120);

    int pad = S(16);
    int ph = S(58);
    L.pill_h = ph;
    L.pill_y = screen_h - S(18) - ph;
    L.pill_x = S(16);
    L.logo_size = S(29);
    L.logo_x = L.pill_x + pad;
    L.logo_y = L.pill_y + (ph - L.logo_size) / 2;

    const char *name = "ReverticalOS";
    L.text_x = L.logo_x + L.logo_size + S(10);
    L.text_baseline = L.pill_y + ph / 2 + S(7);
    int name_w = tw(&g_font_title19, name);

    L.nav_size = S(23);
    L.nav_step = S(15);
    L.nav_y = L.pill_y + (ph - L.nav_size) / 2;
    int nav_start = L.text_x + name_w + S(17);
    for (int i = 0; i < 4; i++)
        L.nav_x[i] = nav_start + i * (L.nav_size + L.nav_step);
    int pill_right = L.nav_x[3] + L.nav_size + pad;
    L.pill_w = pill_right - L.pill_x;

    L.dock_size = 48;
    L.dock_h = ph;
    int gap = S(10);
    L.dock_w = S(12) * 2 + DOCK_APPS * L.dock_size + (DOCK_APPS - 1) * gap;
    L.dock_x = (screen_w - L.dock_w) / 2;
    L.dock_y = L.pill_y;
    L.dock_iy = L.dock_y + (L.dock_h - L.dock_size) / 2;
    for (int i = 0; i < APP_COUNT; i++)
        L.dock_ix[i] = L.dock_x + S(12) + i * (L.dock_size + gap);

    L.card_w = S(202);
    L.card_h = S(126);
    L.card_x = screen_w - S(18) - L.card_w;
    L.card_y = screen_h - S(18) - L.card_h;
    L.top_x = L.card_x + S(8);
    L.top_y = L.card_y + S(8);
    L.top_w = L.card_w - S(16);
    L.top_h = S(52);
    L.bot_x = L.card_x + S(8);
    L.bot_y = L.top_y + L.top_h + S(7);
    L.bot_w = L.card_w - S(16);
    L.bot_h = L.card_h - (L.bot_y - L.card_y) - S(8);
}

static void window_defaults(int app)
{
    Window *w = &wins[app];
    w->w = S(600);
    w->h = S(412);
    if (app == APP_INSTALLER) {
        w->w = S(620);
        w->h = S(540);
    }
    if (w->w > screen_w - S(80)) w->w = screen_w - S(80);
    if (w->h > screen_h - S(150)) w->h = screen_h - S(150);
    int base_x = (screen_w - w->w) / 2;
    int base_y = (screen_h - w->h) / 2 - S(48);
    int offx[APP_COUNT] = { -S(56), S(56), -S(20), S(20), 0, 0 };
    int offy[APP_COUNT] = { -S(18), S(14), -S(6), S(6), 0, 0 };
    w->x = base_x + offx[app];
    w->y = base_y + offy[app];
    w->x = iclamp(w->x, S(24) - w->w, screen_w - S(24));
    w->y = iclamp(w->y, S(18), screen_h - S(110));
    w->open = 0;
    w->minimized = 0;
}

void ui_init(void)
{
    layout_rebuild();

    load_image(&a_logo_sm, ASSET_LOGO_SM);
    load_image(&a_logo_lg, ASSET_LOGO_LG);
    load_image(&a_wordmark, ASSET_WORDMARK);
    load_image(&a_dock[APP_BROWSER], ASSET_DOCK_PLANET);
    load_image(&a_dock[APP_FILES], ASSET_DOCK_FOLDER);
    load_image(&a_dock[APP_MESSAGES], ASSET_DOCK_CHAT);
    load_image(&a_dock[APP_SETTINGS], ASSET_DOCK_SETTINGS);
    load_image(&a_dock[APP_AI], ASSET_DOCK_AI);
    load_image(&a_dock[APP_INSTALLER], ASSET_DOCK_INSTALL);
    load_image(&a_nav[0], ASSET_ICON_SEARCH);
    load_image(&a_nav[1], ASSET_ICON_SWITCH);
    load_image(&a_nav[2], ASSET_ICON_FOLDER);
    load_image(&a_nav[3], ASSET_ICON_BELL);
    load_image(&a_wifi, ASSET_WIFI);
    load_image(&a_bars, ASSET_BARS);
    load_image(&a_cursor, ASSET_CURSOR);

    for (int i = 0; i < APP_COUNT; i++) {
        window_defaults(i);
        zorder[i] = i;
    }

    files_refresh();
    live_env = !installed_marker();

    browser_navigate("about:home", wins[APP_BROWSER].w - S(28), 1);

    ui_now = timer_ms();
    last_rtc_ms = 0;
    clock_update();
    notif_open = 0;
    notif_hover = -1;
    notif_count = 0;
    panels_hidden = 0;
    drag_app = -1;
    start_open = 0;
    start_hover = -1;
    start_sel = 0;
    power_at = 0;
    power_action = 0;
    auth_focus = 0;
    auth_error[0] = 0;
    login_pass[0] = 0;
    login_user[0] = 0;
    inst_stage = 0;
    inst_focus = 0;
    inst_error[0] = 0;
    inst_user[0] = 0;
    inst_pass[0] = 0;
    inst_confirm[0] = 0;

    screen_mode = SCREEN_DESKTOP;
    if (!live_env && accounts_exists() && accounts_count() > 0) {
        accounts_get(0, login_user, sizeof(login_user));
        auth_focus = login_user[0] ? 1 : 0;
        screen_mode = SCREEN_LOGIN;
    }

    notif_push(live_env ? "Live desktop ready - installer available"
                        : "Welcome to ReverticalOS", C_ACCENT);

    NetIf *eth = net_wired();
    if (eth && eth->up && eth->ip) {
        seen_eth = 1;
        char text[72];
        int n = 0;
        const char *p = "Ethernet connected - ";
        while (*p && n < 71) text[n++] = *p++;
        const char *ip = net_ip_text(eth->ip);
        while (*ip && n < 71) text[n++] = *ip++;
        text[n] = 0;
        notif_push(text, C_TEAL);
    }
    if (wifi_connected()) {
        seen_wifi = 1;
        char text[72];
        int n = 0;
        const char *p = "Wi-Fi connected - ";
        while (*p && n < 71) text[n++] = *p++;
        const char *ssid = wifi_ssid();
        while (*ssid && n < 71) text[n++] = *ssid++;
        text[n] = 0;
        notif_push(text, C_PURPLE);
    }

    serial_write("ui: desktop ready\r\n");
    if (screen_mode == SCREEN_LOGIN)
        serial_write("ui: login screen\r\n");
}

/* ------------------------------------------------------------------------- */
/*  Hit testing                                                               */
/* ------------------------------------------------------------------------- */
static int point_in(int x, int y, int w, int h, int px, int py)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int window_visible(int app)
{
    return wins[app].open && !wins[app].minimized;
}

static int app_at(int px, int py)
{
    for (int i = 0; i < APP_COUNT; i++) {
        int app = zorder[i];
        if (!window_visible(app)) continue;
        Window *w = &wins[app];
        if (point_in(w->x, w->y, w->w, w->h, px, py))
            return app;
    }
    return -1;
}

static void focus_app(int app)
{
    int idx = 0;
    for (int i = 0; i < APP_COUNT; i++)
        if (zorder[i] == app) { idx = i; break; }
    for (int i = idx; i > 0; i--)
        zorder[i] = zorder[i - 1];
    zorder[0] = app;
}

static void toggle_app(int app)
{
    Window *w = &wins[app];
    if (!w->open) {
        window_defaults(app);
        w->open = 1;
        w->open_time = ui_now;
    } else if (w->minimized) {
        w->minimized = 0;
        w->open_time = ui_now;
    } else if (zorder[0] == app) {
        w->minimized = 1;
        drag_app = -1;
    }
    focus_app(app);
    panels_hidden = 0;
    notif_open = 0;
}

static int any_visible_window(void)
{
    for (int i = 0; i < APP_COUNT; i++)
        if (window_visible(i)) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  Widgets                                                                   */
/* ------------------------------------------------------------------------- */
static void draw_switch(int x, int y, int on)
{
    int w = S(38), h = S(20);
    gfx_fill_round(x, y, w, h, h / 2, on ? C_ACCENT : 0xC9D4E6, 255);
    int kx = on ? x + w - h / 2 - S(1) : x + h / 2 + S(1);
    gfx_fill_circle(kx, y + h / 2, S(8), 0xFFFFFF, 255);
    if (on) gfx_fill_circle(kx, y + h / 2, S(4), C_ACCENT, 255);
}

static void draw_badge(int x, int y, u32 color, int radius)
{
    gfx_fill_circle(x, y, radius, color, 70);
    gfx_fill_circle(x, y, radius - S(2), color, 255);
    gfx_fill_circle(x - radius / 3, y - radius / 3, radius / 3, 0xFFFFFF, 90);
}

/* ------------------------------------------------------------------------- */
/*  App: Browser                                                              */
/* ------------------------------------------------------------------------- */
#define URL_MAX 120
#define PAGE_MAX 8192
#define PAGE_LINES 240
#define PAGE_COLS 96
#define PAGE_LINKS 8

static char browser_url[URL_MAX] = "about:home";
static char browser_text[PAGE_MAX];
static int  browser_text_len;
static char browser_status[80] = "Ready";
static int  browser_scroll;
static int  browser_loading;

static char browser_lines[PAGE_LINES][PAGE_COLS + 1];
static int  browser_line_count;

static struct {
    char label[44];
    char href[URL_MAX];
} browser_links[PAGE_LINKS];
static int browser_link_count;

static char browser_history[8][URL_MAX];
static int browser_hist_count;
static int browser_hist_pos;

static int browser_focus_url;
static int browser_url_select;   /* next typed character replaces the URL */
static int browser_wrap_w;
static int browser_visible_lines = 12;

static int browser_url_focused(void)
{
    return browser_focus_url && window_visible(APP_BROWSER);
}

static void page_reset(void)
{
    browser_text_len = 0;
    browser_text[0] = 0;
    browser_link_count = 0;
    browser_scroll = 0;
    browser_line_count = 0;
}

static void page_add(const char *s)
{
    while (*s && browser_text_len < PAGE_MAX - 1) {
        browser_text[browser_text_len++] = *s++;
    }
    browser_text[browser_text_len] = 0;
}

static void page_add_line(const char *s)
{
    page_add(s);
    page_add("\n");
}

static void page_add_number(const char *prefix, u32 value)
{
    char tmp[12];
    int n = 0;
    if (!value) tmp[n++] = '0';
    while (value) {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    page_add(prefix);
    char c[2] = { 0, 0 };
    while (n) {
        c[0] = tmp[--n];
        page_add(c);
    }
    page_add("\n");
}

static void page_add_link(const char *label, const char *href)
{
    if (browser_link_count >= PAGE_LINKS)
        return;
    int i = 0;
    while (label[i] && i < 43) {
        browser_links[browser_link_count].label[i] = label[i];
        i++;
    }
    browser_links[browser_link_count].label[i] = 0;
    i = 0;
    while (href[i] && i < URL_MAX - 1) {
        browser_links[browser_link_count].href[i] = href[i];
        i++;
    }
    browser_links[browser_link_count].href[i] = 0;
    browser_link_count++;
}

/* ------------------------------------------------------------------------- */
/*  Very small HTML to text converter                                         */
/* ------------------------------------------------------------------------- */
static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b || !a) return 0;
    }
    return 1;
}

static void html_convert(const char *html, int len)
{
    char out[PAGE_MAX];
    page_reset();
    int olen = 0;
    int i = 0;
    int space = 0;
    int blank = 0;
    char link_href[URL_MAX];
    char link_label[44];
    int link_n = 0;
    int in_link = 0;
    int in_skip = 0;

    link_href[0] = 0;
    link_label[0] = 0;

    while (i < len && olen < PAGE_MAX - 1) {
        char c = html[i];
        if (c == '<') {
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' &&
                html[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' &&
                                        html[i + 2] == '>'))
                    i++;
                i += 3;
                continue;
            }
            int close = 0;
            i++;
            if (i < len && html[i] == '/') { close = 1; i++; }
            const char *rest = html + i;
            if (!in_skip && (starts_with_ci(rest, "script") ||
                             starts_with_ci(rest, "style"))) {
                in_skip = 1;
            }
            if (in_skip && close && (starts_with_ci(rest, "script") ||
                                     starts_with_ci(rest, "style"))) {
                in_skip = 0;
            }

            if (!in_skip && !close && starts_with_ci(rest, "a ")) {
                int k = i + 1;
                while (k + 5 < len && !(html[k] == 'h' &&
                                        starts_with_ci(html + k, "href")))
                    k++;
                if (k + 5 < len) {
                    k += 4;
                    while (k < len && (html[k] == ' ' || html[k] == '=' ||
                                       html[k] == '"' || html[k] == '\''))
                        k++;
                    int h = 0;
                    while (k < len && html[k] != '"' && html[k] != '\'' &&
                           html[k] != '>' && html[k] != ' ' && h < URL_MAX - 1)
                        link_href[h++] = html[k++];
                    link_href[h] = 0;
                    in_link = 1;
                    link_n = 0;
                }
            }
            if (!in_skip && close && starts_with_ci(rest, "a")) {
                if (in_link && link_n > 0) {
                    link_label[link_n] = 0;
                    char num[16];
                    int n = 0;
                    int v = browser_link_count + 1;
                    if (v >= 10) num[n++] = (char)('0' + v / 10);
                    num[n++] = (char)('0' + v % 10);
                    num[n] = 0;
                    if (olen + n + 3 < PAGE_MAX) {
                        out[olen++] = ' ';
                        out[olen++] = '[';
                        for (int q = 0; num[q]; q++) out[olen++] = num[q];
                        out[olen++] = ']';
                    }
                    page_add_link(link_label, link_href);
                }
                in_link = 0;
                link_n = 0;
            }

            if (!in_skip && (close || 1)) {
                if (starts_with_ci(rest, "p") || starts_with_ci(rest, "div") ||
                    starts_with_ci(rest, "br") || starts_with_ci(rest, "tr") ||
                    starts_with_ci(rest, "li") || starts_with_ci(rest, "h1") ||
                    starts_with_ci(rest, "h2") || starts_with_ci(rest, "h3") ||
                    starts_with_ci(rest, "title") ||
                    starts_with_ci(rest, "ul") ||
                    starts_with_ci(rest, "table")) {
                    if (olen > 0 && out[olen - 1] != '\n') {
                        out[olen++] = '\n';
                        blank = 0;
                    }
                    if (!close && (starts_with_ci(rest, "p") ||
                                   starts_with_ci(rest, "h") ||
                                   starts_with_ci(rest, "ul"))) {
                        if (blank < 1 && olen + 1 < PAGE_MAX) {
                            out[olen++] = '\n';
                            blank++;
                        }
                    }
                }
            }

            while (i < len && html[i] != '>') i++;
            i++;
            continue;
        }

        i++;
        if (in_skip)
            continue;
        if (c == '&') {
            const char *semi = html + i;
            int n = 0;
            while (n < 10 && semi + n < html + len && semi[n] != ';') n++;
            if (semi + n < html + len && semi[n] == ';') {
                char ent = 0;
                if (n == 3 && starts_with_ci(semi, "amp")) ent = '&';
                else if (n == 2 && starts_with_ci(semi, "lt")) ent = '<';
                else if (n == 2 && starts_with_ci(semi, "gt")) ent = '>';
                else if (n == 4 && starts_with_ci(semi, "quot")) ent = '"';
                else if (n == 4 && starts_with_ci(semi, "nbsp")) ent = ' ';
                else if (n == 2 && starts_with_ci(semi, "39")) ent = '\'';
                if (ent) { c = ent; i += n + 1; }
            }
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = 1;
            continue;
        }
        if (space) {
            if (olen > 0 && out[olen - 1] != '\n' && out[olen - 1] != ' ')
                out[olen++] = ' ';
            space = 0;
        }
        if (c == '\n') {
            out[olen++] = '\n';
            blank = 0;
            continue;
        }
        if (olen < PAGE_MAX - 1) {
            out[olen++] = c;
            if (in_link && link_n < 43) link_label[link_n++] = c;
            blank = 0;
        }
    }
    if (olen > 0 && out[olen - 1] != '\n')
        out[olen++] = '\n';
    out[olen] = 0;

    for (int k = 0; k < olen; k++)
        browser_text[k] = out[k];
    browser_text[olen] = 0;
    browser_text_len = olen;
    browser_scroll = 0;
    browser_line_count = 0;
}

static void browser_wrap(int maxw)
{
    browser_line_count = 0;
    int i = 0;
    char line[PAGE_COLS + 1];
    int n = 0;

    while (browser_text[i] && browser_line_count < PAGE_LINES) {
        char c = browser_text[i++];
        if (c == '\n') {
            line[n] = 0;
            int q = 0;
            while (q < n && line[q] == ' ') q++;
            int k = 0;
            while (line[q + k] && k < PAGE_COLS) {
                browser_lines[browser_line_count][k] = line[q + k];
                k++;
            }
            browser_lines[browser_line_count][k] = 0;
            browser_line_count++;
            n = 0;
            continue;
        }
        line[n++] = c;
        line[n] = 0;
        if (tw(&g_font_ui13, line) > maxw && n > 1) {
            int cut = n - 1;
            while (cut > 0 && line[cut] != ' ') cut--;
            if (cut <= 0) cut = n - 1;
            line[cut] = 0;
            int k = 0;
            while (line[k] && k < PAGE_COLS) {
                browser_lines[browser_line_count][k] = line[k];
                k++;
            }
            browser_lines[browser_line_count][k] = 0;
            browser_line_count++;
            int rest = 0;
            for (int q = cut + 1; q < n; q++) line[rest++] = line[q];
            n = rest;
            if (n == 0 && browser_text[i] == ' ') i++;
        }
    }
    if (n && browser_line_count < PAGE_LINES) {
        int k = 0;
        while (line[k] && k < PAGE_COLS) {
            browser_lines[browser_line_count][k] = line[k];
            k++;
        }
        browser_lines[browser_line_count][k] = 0;
        browser_line_count++;
    }
}

static int browser_page_maxw(int w)
{
    return w - S(24);
}

static void browser_about(const char *url)
{
    page_reset();
    if (strcmp(url, "about:net") == 0) {
        page_add_line("Network");
        page_add_line("=======");
        page_add_line(net_status_line());
        page_add_line("");
        NetIf *eth = net_wired();
        if (eth) {
            page_add_line("eth0 (Intel PRO/1000, e1000)");
            page_add("  MAC ");
            for (int i = 0; i < 6; i++) {
                const char *hex = "0123456789ABCDEF";
                char pair[3] = { hex[eth->mac[i] >> 4], hex[eth->mac[i] & 15], 0 };
                page_add(pair);
                if (i != 5) page_add(":");
            }
            page_add_line("");
            page_add("  IP  ");
            page_add_line(net_ip_text(eth->ip));
            page_add("  GW  ");
            page_add_line(net_ip_text(eth->gw));
            page_add("  DNS ");
            page_add_line(net_ip_text(eth->dns));
            page_add("  DHCP ");
            page_add_line(eth->dhcp_done ? "lease acquired" : "no lease");
            page_add_number("  frames in: ", eth->rx_frames);
        } else {
            page_add_line("No wired adapter detected.");
        }
        page_add_line("");
        NetIf *w = net_wifi();
        if (w) {
            page_add_line("wlan0 (simulated radio)");
            page_add("  IP  ");
            page_add_line(net_ip_text(w->ip));
            page_add("  state ");
            page_add_line(wifi_status());
        }
    } else if (strcmp(url, "about:wifi") == 0) {
        page_add_line("Wi-Fi");
        page_add_line("=====");
        page_add_line(wifi_status());
        if (wifi_connected()) {
            page_add_line("");
            page_add("  SSID ");
            page_add_line(wifi_ssid());
            page_add_line("  security WPA2-PSK");
            page_add_line("  PMK PBKDF2-HMAC-SHA1, 4096 rounds");
            page_add_line("  handshake four-way, HMAC-SHA1 MIC");
            page_add_line("");
            page_add_line("Open http://revertical.local/ to browse the");
            page_add_line("access point's built-in site.");
        } else {
            page_add_line("");
            page_add_line("Open Settings and connect to a network.");
        }
    } else {
        page_add_line("ReverticalOS Browser");
        page_add_line("===================");
        page_add_line("");
        page_add_line("Type an http:// address in the bar above and press Enter.");
        page_add_line("The stack is e1000 + ARP + IPv4 + DHCP + DNS + TCP + HTTP,");
        page_add_line("all written from scratch. No TLS yet, so use http:// URLs.");
        page_add_line("");
        page_add("Network: ");
        page_add_line(net_status_line());
        page_add_line("");
        page_add_line("Links");
        page_add_link("RevertiaNet (Wi-Fi intranet)", "http://revertical.local/");
        page_add_link("Applications on RevertiaNet", "http://revertical.local/apps");
        page_add_link("neverssl.com (wired HTTP test)", "http://neverssl.com/");
        page_add_link("Network details", "about:net");
        page_add_link("Wi-Fi state", "about:wifi");
    }
}

static void browser_history_push(const char *url)
{
    if (browser_hist_count > 0 &&
        strcmp(browser_history[browser_hist_pos], url) == 0)
        return;
    if (browser_hist_pos + 1 < browser_hist_count)
        browser_hist_count = browser_hist_pos + 1;
    if (browser_hist_count >= 8) {
        for (int i = 1; i < 8; i++)
            memcpy(browser_history[i - 1], browser_history[i], URL_MAX);
        browser_hist_count = 7;
        browser_hist_pos = 6;
    }
    int i = 0;
    while (url[i] && i < URL_MAX - 1) {
        browser_history[browser_hist_count][i] = url[i];
        i++;
    }
    browser_history[browser_hist_count][i] = 0;
    browser_hist_pos = browser_hist_count;
    browser_hist_count++;
}

static void browser_navigate(const char *url, int w, int record)
{
    if (!url || !url[0])
        return;

    int i = 0;
    while (url[i] && i < URL_MAX - 1) {
        browser_url[i] = url[i];
        i++;
    }
    browser_url[i] = 0;
    browser_url_select = 0;
    browser_scroll = 0;
    browser_loading = 1;

    if (starts_with_ci(browser_url, "about:")) {
        browser_about(browser_url);
        int n = 0;
        const char *s = "Local page";
        while (s[n]) { browser_status[n] = s[n]; n++; }
        browser_status[n] = 0;
    } else {
        char full[URL_MAX + 8];
        if (starts_with_ci(browser_url, "http://")) {
            memcpy(full, browser_url, (size_t)i + 1);
        } else {
            const char *scheme = "http://";
            int k = 0;
            while (scheme[k]) { full[k] = scheme[k]; k++; }
            memcpy(full + k, browser_url, (size_t)i + 1);
        }

        char err[64];
        char body[PAGE_MAX];
        int r = net_http_url(full, body, sizeof(body), err, sizeof(err), 5000);
        if (r >= 0) {
            html_convert(body, r);
            int n = 0;
            const char *s = "Loaded ";
            while (s[n]) { browser_status[n] = s[n]; n++; }
            for (int k = 0; browser_url[k] && n < 63; k++)
                browser_status[n++] = browser_url[k];
            browser_status[n] = 0;
        } else if (r <= -300) {
            page_reset();
            page_add_line("HTTP error");
            page_add_number("Status ", (u32)(-r));
            page_add_line(browser_url);
            page_add_line("");
            page_add_line("The server answered, but not with a 200 page.");
            page_add_line("Redirects and HTTPS are not implemented, so a");
            page_add_line("site that upgrades to https:// stops here.");
        } else {
            page_reset();
            page_add_line("Could not load the page");
            page_add_line("======================");
            page_add_line(browser_url);
            page_add_line("");
            page_add_line(err);
            page_add_line("");
            page_add_line("Check the network:");
            page_add_line(net_status_line());
            page_add_line("");
            page_add_line("Wi-Fi: ");
            page_add_line(wifi_status());
            page_add_line("");
            page_add_line("Only http:// URLs work; there is no TLS stack yet.");
        }
        browser_status[0] = 0;
        int n = 0;
        if (r >= 0) {
            const char *s = "Loaded ";
            while (s[n]) { browser_status[n] = s[n]; n++; }
        } else {
            const char *s = "Failed: ";
            while (s[n]) { browser_status[n] = s[n]; n++; }
        }
        for (int k = 0; browser_url[k] && n < 79; k++)
            browser_status[n++] = browser_url[k];
        browser_status[n] = 0;
    }

    browser_loading = 0;
    browser_wrap(browser_page_maxw(w));
    if (record)
        browser_history_push(browser_url);
}

void app_browser_body(int x, int y, int w, int h)
{
    int nav_w = S(30);
    int bx = x + S(2);

    /* toolbar */
    gfx_fill_round(bx, y + S(4), nav_w, S(26), S(8), 0xFFFFFF, 200);
    int ax = bx + nav_w / 2;
    line(ax + S(4), y + S(11), ax - S(3), y + S(17), C_INK, 235);
    line(ax - S(3), y + S(17), ax + S(4), y + S(23), C_INK, 235);

    bx += nav_w + S(6);
    gfx_fill_round(bx, y + S(4), nav_w, S(26), S(8), 0xFFFFFF, 120);
    ax = bx + nav_w / 2;
    line(ax - S(4), y + S(11), ax + S(3), y + S(17), C_INK, 160);
    line(ax + S(3), y + S(17), ax - S(4), y + S(23), C_INK, 160);

    bx += nav_w + S(6);
    gfx_fill_round(bx, y + S(4), nav_w, S(26), S(8), 0xFFFFFF, 200);
    ax = bx + nav_w / 2;
    gfx_fill_round(ax - S(5), y + S(11), S(10), S(9), S(2), C_INK, 220);
    gfx_fill_rect(ax - S(3), y + S(20), S(6), S(2), C_INK);

    bx += nav_w + S(6);
    int ubw = x + w - bx - S(2);
    int focused = browser_url_focused();
    gfx_fill_round(bx, y + S(4), ubw, S(26), S(8),
                   focused ? 0xFFFFFF : 0xF2F6FD, 255);
    if (focused)
        gfx_panel(bx, y + S(4), ubw, S(26), S(8), 0xFFFFFF, 0, C_ACCENT, 200);
    const char *shown = browser_url;
    while (*shown && tw(&g_font_ui13, shown) > ubw - S(20))
        shown++;
    if (focused && browser_url_select)
        gfx_fill_round(bx + S(8), y + S(8),
                       tw(&g_font_ui13, shown) + S(4), S(18), S(4),
                       0xDAE6FF, 235);
    text_at(&g_font_ui13, bx + S(10), y + S(21), shown, C_INK, 240);
    if (focused && !browser_url_select && ((ui_now / 500) & 1)) {
        int cw = tw(&g_font_ui13, shown);
        int cx = bx + S(10) + cw;
        if (cx > bx + ubw - S(6)) cx = bx + ubw - S(6);
        gfx_fill_rect(cx + 1, y + S(8), 1, S(17), C_ACCENT);
    }

    text_fit(&g_font_ui13, x + S(2), y + S(44), browser_status,
             w - S(6), 0x68748F, 215);
    gfx_fill_rect(x, y + S(50), w, 1, C_LINE);

    int maxw = browser_page_maxw(w);
    if (maxw != browser_wrap_w) {
        browser_wrap_w = maxw;
        browser_wrap(maxw);
    }

    int content_y = y + S(56);
    int link_h = S(20);
    int link_area = browser_link_count ? (browser_link_count * link_h + S(18)) : 0;
    int content_h = h - (content_y - y) - link_area - S(6);
    int line_h = S(17);
    int visible = content_h / line_h;
    if (visible < 1) visible = 1;
    browser_visible_lines = visible;

    if (browser_scroll > browser_line_count - visible)
        browser_scroll = browser_line_count - visible;
    if (browser_scroll < 0)
        browser_scroll = 0;

    if (browser_loading) {
        text_at(&g_font_ui15sb, x + S(4), content_y + S(20),
                "Loading...", C_INK, 240);
    }

    gfx_set_clip(x, content_y, w, content_h);
    for (int i = 0; i < visible; i++) {
        int li = browser_scroll + i;
        if (li >= browser_line_count) break;
        u32 color = 0x3A4763;
        const char *s = browser_lines[li];
        if (s[0] == '=' && s[1] == '=') color = 0x8A94AE;
        else if (s[0] == '#' || (s[0] >= '0' && s[0] <= '9' && s[1] == '.'))
            color = 0x16213B;
        else if (s[0] == ' ' && s[1] == ' ') color = 0x58658A;
        text_at(&g_font_ui13, x + S(4), content_y + S(13) + i * line_h,
                s, color, 240);
    }
    gfx_reset_clip();

    if (browser_line_count > visible) {
        int sy = content_y + S(2);
        int sh = content_h - S(4);
        int total = browser_line_count;
        int th = sh * visible / total;
        if (th < S(16)) th = S(16);
        int ty = sy + (sh - th) * browser_scroll / (total - visible);
        gfx_fill_round(x + w - S(4), sy, S(3), sh, S(2), 0xE2E9F5, 200);
        gfx_fill_round(x + w - S(4), ty, S(3), th, S(2), 0x9FB0CC, 230);
    }

    if (browser_link_count) {
        int ly = y + h - link_area - S(2);
        gfx_fill_rect(x, ly, w, 1, C_LINE);
        text_tracked(&g_font_ui13, x + S(2), ly + S(16), "LINKS",
                     0x8A94AE, 220, 2);
        for (int i = 0; i < browser_link_count; i++) {
            int ry = ly + S(22) + i * link_h;
            int hot = 0;
            if (!browser_focus_url && mouse_x >= x && mouse_x < x + w &&
                mouse_y >= ry && mouse_y < ry + link_h)
                hot = 1;
            if (hot)
                gfx_fill_round(x, ry, w, link_h - S(2), S(5), 0xEFF4FF, 220);
            char num[6];
            num[0] = '[';
            num[1] = (char)('1' + i);
            num[2] = ']';
            num[3] = 0;
            text_at(&g_font_ui13, x + S(4), ry + S(14), num, C_ACCENT, 235);
            text_fit(&g_font_ui13, x + S(30), ry + S(14),
                     browser_links[i].label, w - S(120), C_INK, 240);
            char dest[40];
            path_copy(dest, browser_links[i].href, 39);
            text_fit(&g_font_ui13, x + w - S(6) - tw(&g_font_ui13, dest),
                     ry + S(14), dest, S(140), 0x93A0B8, 200);
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  App: Browser (input)                                                      */
/* ------------------------------------------------------------------------- */
static void browser_url_trim(void)
{
    if (browser_url_select) {
        browser_url[0] = 0;
        browser_url_select = 0;
        return;
    }
    int n = (int)strlen(browser_url);
    if (n > 0) browser_url[n - 1] = 0;
}

static void browser_url_append(int c)
{
    if (browser_url_select) {
        browser_url[0] = 0;
        browser_url_select = 0;
    }
    int n = (int)strlen(browser_url);
    if (n < URL_MAX - 2) {
        browser_url[n] = (char)c;
        browser_url[n + 1] = 0;
    }
}

static void browser_scroll_step(int delta)
{
    browser_scroll += delta;
    if (browser_scroll < 0)
        browser_scroll = 0;
    if (browser_scroll > browser_line_count - browser_visible_lines)
        browser_scroll = browser_line_count - browser_visible_lines;
    if (browser_scroll < 0)
        browser_scroll = 0;
}

/* Resolve an href from the current page into an absolute URL. */
static void browser_resolve(const char *href, char *out, int max)
{
    int n = 0;
    if (starts_with_ci(href, "http://") || starts_with_ci(href, "https://") ||
        starts_with_ci(href, "about:")) {
        while (href[n] && n < max - 1) { out[n] = href[n]; n++; }
        out[n] = 0;
        return;
    }
    if (!href[0] || href[0] == '#') {
        while (browser_url[n] && n < max - 1) { out[n] = browser_url[n]; n++; }
        out[n] = 0;
        return;
    }

    const char *base = browser_url;
    if (!starts_with_ci(base, "http://")) {
        out[0] = 0;
        return;
    }

    int auth = 7;                    /* past "http://" */
    int blen = (int)strlen(base);

    if (href[0] == '/') {
        /* Root-relative: keep scheme and authority only. */
        while (auth < blen && base[auth] != '/') auth++;
        for (int i = 0; i < auth && n < max - 1; i++) out[n++] = base[i];
        for (int i = 0; href[i] && n < max - 1; i++) out[n++] = href[i];
        out[n] = 0;
        return;
    }

    /* Relative: replace everything after the last '/' of the base path. */
    int slash = -1;
    for (int i = blen - 1; i >= auth; i--) {
        if (base[i] == '/') { slash = i; break; }
    }
    if (slash >= 0) {
        for (int i = 0; i <= slash && n < max - 1; i++) out[n++] = base[i];
    } else {
        for (int i = 0; i < blen && n < max - 1; i++) out[n++] = base[i];
        if (n < max - 1) out[n++] = '/';
    }
    for (int i = 0; href[i] && n < max - 1; i++) out[n++] = href[i];
    out[n] = 0;
}

static void browser_goto_link(int index)
{
    if (index < 0 || index >= browser_link_count)
        return;
    char url[URL_MAX];
    browser_resolve(browser_links[index].href, url, sizeof(url));
    if (!url[0])
        return;
    browser_navigate(url, wins[APP_BROWSER].w - S(28), 1);
}

static int browser_link_hit(int mx, int my, int x, int y, int w, int h)
{
    if (browser_link_count == 0)
        return -1;
    int ly = y + h - (browser_link_count * S(20) + S(18)) - S(2);
    for (int i = 0; i < browser_link_count; i++) {
        int ry = ly + S(22) + i * S(20);
        if (mx >= x && mx < x + w && my >= ry && my < ry + S(20))
            return i;
    }
    return -1;
}

static int app_browser_click(int x, int y, int w, int h, int mx, int my)
{
    int nav_w = S(30);
    int step = nav_w + S(6);

    if (mx >= x + S(2) && mx < x + S(2) + nav_w &&
        my >= y + S(4) && my < y + S(30)) {
        if (browser_hist_pos > 0) {
            browser_hist_pos--;
            browser_navigate(browser_history[browser_hist_pos],
                             w, 0);
        }
        return 1;
    }
    if (mx >= x + S(2) + step && mx < x + S(2) + step + nav_w &&
        my >= y + S(4) && my < y + S(30)) {
        if (browser_hist_pos + 1 < browser_hist_count) {
            browser_hist_pos++;
            browser_navigate(browser_history[browser_hist_pos],
                             w, 0);
        }
        return 1;
    }
    if (mx >= x + S(2) + step * 2 && mx < x + S(2) + step * 2 + nav_w &&
        my >= y + S(4) && my < y + S(30)) {
        browser_navigate("about:home", w, 1);
        return 1;
    }

    int ubx = x + S(2) + step * 3;
    int ubw = x + w - ubx - S(2);
    if (mx >= ubx && mx < ubx + ubw && my >= y + S(4) && my < y + S(30)) {
        browser_focus_url = 1;
        browser_url_select = 1;
        return 1;
    }

    int link = browser_link_hit(mx, my, x, y, w, h);
    if (link >= 0) {
        browser_focus_url = 0;
        browser_goto_link(link);
        return 1;
    }

    if (my > y + S(50)) {
        browser_focus_url = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  App: Program Output (RVEX runner)                                         */
/* ------------------------------------------------------------------------- */
static char console_title[64] = "Program Output";
static int console_source = -1;

void app_console_body(int x, int y, int w, int h)
{
    text_at(&g_font_ui15sb, x + S(2), y + S(18), console_title, C_INK, 245);

    if (console_source < 0) {
        text_at(&g_font_ui13, x + S(2), y + S(42),
                "Open Files and double-click a .rvx executable.", C_MUTED, 215);
        text_at(&g_font_ui13, x + S(2), y + S(60),
                "RVEX images are loaded by the kernel module loader;", C_MUTED, 215);
        text_at(&g_font_ui13, x + S(2), y + S(76),
                "RVLB libraries in /System/Libs supply their imports.", C_MUTED, 215);
        return;
    }

    if (console_source == 0) {
        text_at(&g_font_ui13, x + S(2), y + S(40), "rvmod error:", C_RED, 235);
        text_at(&g_font_ui13, x + S(2), y + S(58), rvmod_error(), 0x9B2C31, 235);
        return;
    }

    gfx_panel(x, y + S(28), w, h - S(34), S(9), 0x0B1424, 250, 0x24314D, 220);
    gfx_set_clip(x, y + S(28), w, h - S(34));

    const char *text = rvmod_output();
    int line = 0;
    int col = 0;
    int line_h = S(15);
    int max_cols = (w - S(16)) / imax(1, S(7));
    if (max_cols < 8) max_cols = 8;
    (void)max_cols;

    int ty = y + S(46);
    char buf[PAGE_COLS + 1];
    for (int i = 0; text[i] && line < 40; i++) {
        char c = text[i];
        if (c == '\n') {
            buf[col] = 0;
            text_at(&g_font_ui13, x + S(8), ty + line * line_h, buf,
                    0xBFE3FF, 240);
            line++;
            col = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if (col < PAGE_COLS) buf[col++] = c;
        if (col >= (w - S(16)) / imax(1, S(7))) {
            buf[col] = 0;
            text_at(&g_font_ui13, x + S(8), ty + line * line_h, buf,
                    0xBFE3FF, 240);
            line++;
            col = 0;
            if (line >= 40) break;
        }
    }
    if (col && line < 40) {
        buf[col] = 0;
        text_at(&g_font_ui13, x + S(8), ty + line * line_h, buf,
                0xBFE3FF, 240);
    }
    gfx_reset_clip();

    text_at(&g_font_ui13, x + S(2), y + h - S(4),
            "RVEX module ran in ring 0 on a private stack", 0x8A94AE, 200);
}

/* ------------------------------------------------------------------------- */
/*  App: Files                                                                */
/* ------------------------------------------------------------------------- */
static void folder_tile(int x, int y, int size, u32 tint)
{
    gfx_fill_round(x, y, size, size, S(8), tint, 60);
    gfx_fill_round(x + size / 4, y + size / 3, size / 2, size / 3,
                   S(3), tint, 220);
    gfx_fill_round(x + size / 4, y + size / 2, size / 2, size / 3,
                   S(3), tint, 255);
}

static void doc_tile(int x, int y, int size)
{
    gfx_fill_round(x, y, size, size, S(6), 0xFFFFFF, 235);
    gfx_fill_round(x + size / 4, y + size / 5, size / 2, S(2),
                   S(1), 0xB9C5DA, 255);
    gfx_fill_round(x + size / 4, y + size / 2, size / 2, S(2),
                   S(1), 0xB9C5DA, 255);
    gfx_fill_round(x + size / 4, y + size * 3 / 5, size / 3, S(2),
                   S(1), 0xB9C5DA, 255);
}

static void path_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void path_join(char *out, const char *dir, const char *name)
{
    int n = 0;
    while (dir[n] && n < 120) {
        out[n] = dir[n];
        n++;
    }
    if (n == 0 || out[n - 1] != '/')
        out[n++] = '/';
    int i = 0;
    while (name[i] && n < 158) out[n++] = name[i++];
    out[n] = 0;
}

static void text_fit(const FontDef *f, int x, int baseline, const char *s,
                     int maxw, u32 color, u8 alpha)
{
    if (tw(f, s) <= maxw) {
        text_at(f, x, baseline, s, color, alpha);
        return;
    }
    char buf[68];
    int n = 0;
    int len = (int)strlen(s);
    if (len > 63) len = 63;
    while (n < len) {
        buf[n] = s[n];
        buf[n + 1] = 0;
        if (tw(f, buf) > maxw - tw(f, "...")) break;
        n++;
    }
    buf[n] = 0;
    int k = 0;
    while (k < 3) buf[n++] = '.', k++;
    buf[n] = 0;
    text_at(f, x, baseline, buf, color, alpha);
}

static int draw_wrapped(const FontDef *f, int x, int y, int maxw,
                        const char *s, u32 color, u8 alpha, int line_h,
                        int max_lines)
{
    char line[128];
    int n = 0, lines = 0;
    int cx = x;
    int i = 0;

    while (s[i] && lines < max_lines) {
        char word[64];
        int wn = 0;
        if (s[i] == '\n') {
            line[n] = 0;
            if (n) text_at(f, x, y + lines * line_h, line, color, alpha);
            lines++;
            n = 0;
            cx = x;
            i++;
            continue;
        }
        while (s[i] && s[i] != ' ' && s[i] != '\n' && wn < 62) word[wn++] = s[i++];
        word[wn] = 0;
        int ww = tw(f, word);
        if (ww > maxw) ww = maxw;
        int space = n ? tw(f, " ") : 0;
        if (n && cx + space + ww > x + maxw) {
            line[n] = 0;
            text_at(f, x, y + lines * line_h, line, color, alpha);
            lines++;
            n = 0;
            cx = x;
            if (lines >= max_lines) break;
            space = 0;
        }
        if (n && space) {
            if (n < 126) line[n++] = ' ';
            cx += space;
        }
        for (int k = 0; k < wn && n < 126; k++) line[n++] = word[k];
        cx += ww;
        while (s[i] == ' ') i++;
    }
    if (n && lines < max_lines) {
        line[n] = 0;
        text_at(f, x, y + lines * line_h, line, color, alpha);
        lines++;
    }
    return lines;
}

static void fmt_size(char *out, u32 bytes)
{
    if (bytes >= 1024u * 1024u) {
        u32 whole = bytes / (1024u * 1024u);
        u32 frac = (bytes / (1024u * 102u)) % 10u;
        out[0] = (char)('0' + (whole / 10) % 10);
        out[1] = (char)('0' + whole % 10);
        out[2] = '.';
        out[3] = (char)('0' + frac);
        out[4] = ' ';
        out[5] = 'M';
        out[6] = 'B';
        out[7] = 0;
    } else if (bytes >= 1024) {
        u32 whole = bytes / 1024u;
        u32 frac = (bytes / 102u) % 10u;
        out[0] = (char)('0' + (whole / 100) % 10);
        out[1] = (char)('0' + (whole / 10) % 10);
        out[2] = (char)('0' + whole % 10);
        out[3] = '.';
        out[4] = (char)('0' + frac);
        out[5] = ' ';
        out[6] = 'K';
        out[7] = 'B';
        out[8] = 0;
    } else {
        u32 v = bytes;
        char tmp[12];
        int n = 0;
        if (v == 0) tmp[n++] = '0';
        while (v) {
            tmp[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        int k = 0;
        while (n) out[k++] = tmp[--n];
        out[k++] = ' ';
        out[k++] = 'B';
        out[k] = 0;
    }
}

static void app_files_body(int x, int y, int w, int h);

/* ------------------------------------------------------------------------- */
/*  App: Files (RVFS browser)                                                 */
/* ------------------------------------------------------------------------- */
#define FS_MAX_ENTRIES 32
static char fs_path[160] = "/";
static RvfsEntry fs_entries[FS_MAX_ENTRIES];
static int fs_entry_count;
static int fs_selected = -1;
static char fs_preview[1200];
static int fs_preview_len;
static int fs_preview_kind;      /* 0 text, 1 RVEX, 2 RVLB */
static char fs_preview_name[72];
static int fs_ready;
static u64 fs_last_click;
static int fs_last_click_index = -1;

static int has_ext(const char *name, const char *ext)
{
    int n = (int)strlen(name);
    int m = (int)strlen(ext);
    if (n <= m + 1 || name[n - m - 1] != '.')
        return 0;
    for (int i = 0; i < m; i++) {
        char a = name[n - m + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void preview_text(const char *text)
{
    int n = 0;
    while (text[n] && n < (int)sizeof(fs_preview) - 1) {
        fs_preview[n] = text[n];
        n++;
    }
    fs_preview[n] = 0;
    fs_preview_len = n;
}

static int installed_marker(void)
{
    RvfsStat st;
    return rvfs_mounted() && rvfs_stat("/System/installed", &st) == RVFS_OK;
}

static int dir_first_cmp(const RvfsEntry *a, const RvfsEntry *b)
{
    if (a->type != b->type)
        return a->type == 2 ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void files_refresh(void)
{
    fs_entry_count = 0;
    fs_selected = -1;
    fs_preview_len = 0;
    fs_preview_kind = 0;
    fs_preview_name[0] = 0;
    fs_ready = rvfs_mounted();
    if (!fs_ready)
        return;

    for (int i = 0; i < FS_MAX_ENTRIES; i++) {
        RvfsEntry e;
        if (rvfs_readdir(fs_path, (u32)i, &e) <= 0)
            break;
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;
        fs_entries[fs_entry_count++] = e;
    }

    for (int i = 1; i < fs_entry_count; i++) {
        RvfsEntry key = fs_entries[i];
        int j = i - 1;
        while (j >= 0 && dir_first_cmp(&fs_entries[j], &key) > 0) {
            fs_entries[j + 1] = fs_entries[j];
            j--;
        }
        fs_entries[j + 1] = key;
    }
}

static void files_enter(const char *name)
{
    char next[160];
    path_join(next, fs_path, name);
    if (strlen(next) >= sizeof(fs_path) - 1)
        return;
    path_copy(fs_path, next, sizeof(fs_path));
    files_refresh();
}

static void files_up(void)
{
    if (strcmp(fs_path, "/") == 0)
        return;
    int n = (int)strlen(fs_path);
    while (n > 1 && fs_path[n - 1] != '/')
        n--;
    if (n > 1)
        n--;
    fs_path[n] = 0;
    files_refresh();
}

static void files_preview(int index)
{
    fs_selected = index;
    fs_preview_len = 0;
    fs_preview_name[0] = 0;
    if (index < 0 || index >= fs_entry_count)
        return;
    RvfsEntry *e = &fs_entries[index];
    if (e->type != 1)
        return;

    path_copy(fs_preview_name, e->name, sizeof(fs_preview_name));

    if (has_ext(e->name, "rvx") || has_ext(e->name, "lib")) {
        int exe = has_ext(e->name, "rvx");
        fs_preview_kind = exe ? 1 : 2;
        char info[400];
        int n = 0;
        const char *head = exe
            ? "RVEX executable\n\nEntry point, image size, stack size and\n"
              "named imports.\n\nDouble-click this file to load and run it.\n"
              "The kernel rebases the image, resolves imports\n"
              "against its API and /System/Libs, then calls\n"
              "_start on a private stack.\n\n"
            : "RVLB relocatable library\n\nFlat image linked at address 0\n"
              "with named exports. Libraries in /System/Libs\n"
              "are loaded automatically before an executable\n"
              "runs, so any .rvx can import their symbols.\n\n";
        while (head[n] && n < (int)sizeof(info) - 1) {
            info[n] = head[n];
            n++;
        }
        info[n] = 0;
        preview_text(info);
        return;
    }

    fs_preview_kind = 0;
    char path[192];
    path_join(path, fs_path, e->name);
    int n = rvfs_read(path, 0, fs_preview, sizeof(fs_preview) - 1);
    if (n < 0)
        n = 0;
    fs_preview_len = n;
    fs_preview[n] = 0;
}

static void files_launch(int index)
{
    if (index < 0 || index >= fs_entry_count)
        return;
    RvfsEntry *e = &fs_entries[index];

    char path[192];
    path_join(path, fs_path, e->name);

    serial_write("ui: launching ");
    serial_write(path);
    serial_write("\r\n");

    int rc = rvmod_run(path);
    path_copy(console_title, e->name, sizeof(console_title));
    console_source = (rc == 0) ? 1 : 0;

    char note[80];
    int n = 0;
    const char *prefix = rc == 0 ? "Ran " : "Program failed - ";
    while (*prefix && n < 79) note[n++] = *prefix++;
    for (int i = 0; e->name[i] && n < 79; i++) note[n++] = e->name[i];
    note[n] = 0;
    notif_push(note, rc == 0 ? C_GREEN : C_RED);

    if (wins[APP_CONSOLE].open) {
        wins[APP_CONSOLE].minimized = 0;
        wins[APP_CONSOLE].open_time = ui_now;
        focus_app(APP_CONSOLE);
    } else {
        toggle_app(APP_CONSOLE);
    }
}

static int app_files_click(int x, int y, int w, int h, int mx, int my)
{
    if (!fs_ready)
        return 1;
    if (point_in(x, y + S(6), S(28), S(28), mx, my)) {
        files_up();
        return 1;
    }

    int listw = w * 55 / 100;
    int ry = y + S(52);
    int rh = S(38);
    int maxrows = (h - S(78)) / rh;
    if (maxrows < 1) maxrows = 1;
    for (int i = 0; i < fs_entry_count && i < maxrows; i++) {
        int yy = ry + i * rh;
        if (mx >= x && mx < x + listw && my >= yy && my < yy + rh) {
            if (fs_entries[i].type == 2) {
                fs_last_click_index = -1;
                files_enter(fs_entries[i].name);
            } else if (has_ext(fs_entries[i].name, "rvx")) {
                u64 now = timer_ms();
                if (fs_last_click_index == i && now - fs_last_click < 600) {
                    fs_last_click_index = -1;
                    files_launch(i);
                } else {
                    files_preview(i);
                    fs_last_click_index = i;
                    fs_last_click = now;
                }
            } else {
                fs_last_click_index = -1;
                files_preview(i);
            }
            return 1;
        }
    }
    return 1;
}

static void app_files_body(int x, int y, int w, int h)
{
    if (!fs_ready) {
        text_at(&g_font_title19, x + S(4), y + S(40), "No volume", C_INK, 240);
        text_at(&g_font_ui13, x + S(4), y + S(62),
                "An RVFS volume was not found on any ATA disk.", C_MUTED, 220);
        return;
    }

    const RvfsVolume *vol = rvfs_volume();

    /* toolbar */
    int us = S(28);
    gfx_fill_round(x, y + S(6), us, us, S(9), 0xFFFFFF, 190);
    line(x + S(9), y + S(22), x + us / 2, y + S(16), C_INK, 235);
    line(x + us / 2, y + S(16), x + us - S(9), y + S(22), C_INK, 235);

    text_tracked(&g_font_ui13, x + S(38), y + S(26), "RVFS", 0x8A94AE, 230, 2);
    int px = x + S(38) + tw(&g_font_ui13, "RVFS") + S(10);
    text_fit(&g_font_ui13, px, y + S(26), fs_path, w - (px - x) - S(120),
             C_INK, 245);

    if (vol) {
        char info[32];
        u32 free_mb = vol->free_blocks * vol->block_size / (1024u * 1024u);
        u32 k = 0;
        const char *p = "free ";
        while (*p) info[k++] = *p++;
        char num[12];
        int n = 0;
        if (free_mb == 0) num[n++] = '0';
        while (free_mb) { num[n++] = (char)('0' + free_mb % 10); free_mb /= 10; }
        while (n) info[k++] = num[--n];
        info[k++] = ' '; info[k++] = 'M'; info[k++] = 'B'; info[k] = 0;
        text_at(&g_font_ui13, x + w - tw(&g_font_ui13, info),
                y + S(26), info, C_MUTED, 220);
    }

    gfx_fill_rect(x, y + S(42), w, 1, C_LINE);

    int listw = w * 55 / 100;
    int ry = y + S(52);
    int rh = S(38);
    int maxrows = (h - S(78)) / rh;
    if (maxrows < 1) maxrows = 1;

    for (int i = 0; i < fs_entry_count && i < maxrows; i++) {
        int yy = ry + i * rh;
        RvfsEntry *e = &fs_entries[i];
        if (i == fs_selected)
            gfx_fill_round(x - S(2), yy - S(2), listw, rh - S(2), S(9),
                           0xE8F0FE, 240);
        if (e->type == 2)
            folder_tile(x + S(4), yy + S(5), S(24), C_ACCENT);
        else
            doc_tile(x + S(4), yy + S(5), S(24));

        text_fit(&g_font_ui13, x + S(38), yy + S(20), e->name,
                 listw - S(66), C_INK, 245);
        if (e->type == 1) {
            char sz[12];
            fmt_size(sz, e->size);
            text_at(&g_font_ui13, x + listw - tw(&g_font_ui13, sz) - S(4),
                    yy + S(20), sz, 0x93A0B8, 210);
        }
    }

    if (fs_entry_count == 0)
        text_at(&g_font_ui13, x + S(6), ry + S(20), "Empty folder",
                C_MUTED, 180);

    /* preview pane */
    int pvx = x + listw + S(8);
    int pvw = w - listw - S(8);
    gfx_panel(pvx, y + S(50), pvw, h - S(76), S(12), 0xFFFFFF, 150,
              0xDCE4F2, 190);

    if (fs_selected >= 0 && fs_selected < fs_entry_count) {
        RvfsEntry *e = &fs_entries[fs_selected];
        text_tracked(&g_font_ui13, pvx + S(12), y + S(70),
                     e->type == 2 ? "FOLDER" : "PREVIEW", 0x8A94AE, 225, 2);
        text_fit(&g_font_ui15sb, pvx + S(12), y + S(92), e->name,
                 pvw - S(24), C_INK, 245);
        if (e->type == 2) {
            char sub[40];
            path_copy(sub, "Open to browse this folder.", 39);
            text_at(&g_font_ui13, pvx + S(12), y + S(112), sub, C_MUTED, 210);
        } else if (fs_preview_len > 0) {
            if (fs_preview_kind) {
                const char *tag = fs_preview_kind == 1 ? "RVEX" : "RVLB";
                int tagw = tw(&g_font_ui13, tag) + S(16);
                gfx_panel(pvx + S(12), y + S(104), tagw, S(20), S(5),
                          fs_preview_kind == 1 ? 0xE7F0FF : 0xE6FAF9,
                          255, fs_preview_kind == 1 ? C_ACCENT : C_TEAL, 200);
                text_center(&g_font_ui13, pvx + S(12) + tagw / 2,
                            y + S(118), tag,
                            fs_preview_kind == 1 ? C_ACCENT : 0x0E9C9E,
                            245);
            }
            draw_wrapped(&g_font_ui13, pvx + S(12), y + S(140), pvw - S(24),
                         fs_preview, 0x4A5670, 230, S(17),
                         imax(1, (h - S(174)) / S(17)));
        } else {
            text_at(&g_font_ui13, pvx + S(12), y + S(112),
                    "Empty file", C_MUTED, 200);
        }
    } else {
        text_at(&g_font_ui13, pvx + S(12), y + S(70),
                "Select a file to preview it.", C_MUTED, 200);
    }

    char foot[48];
    u32 n = 0;
    char num[8];
    int k = 0, v = fs_entry_count;
    if (v == 0) num[k++] = '0';
    while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) foot[n++] = num[--k];
    const char *items = " items";
    while (*items) foot[n++] = *items++;
    if (vol) {
        const char *tail = "  ·  RVFS";
        while (*tail) foot[n++] = *tail++;
    }
    foot[n] = 0;
    text_at(&g_font_ui13, x + S(2), y + h - S(4), foot, C_MUTED, 180);
}

/* ------------------------------------------------------------------------- */
/*  App: Messages                                                             */
/* ------------------------------------------------------------------------- */
/*  App: Messages                                                             */
/* ------------------------------------------------------------------------- */
static void bubble(int x, int y, int maxw, const char *l1, const char *l2,
                   int outgoing)
{
    int padx = S(14), pady = S(10);
    int w1 = tw(&g_font_ui13, l1);
    int w2 = l2 ? tw(&g_font_ui13, l2) : 0;
    int bw = (w1 > w2 ? w1 : w2) + padx * 2;
    if (bw > maxw) bw = maxw;
    int bh = (l2 ? S(20) : 0) + S(15) + pady * 2;
    if (!outgoing) {
        gfx_fill_round(x, y, bw, bh, S(13), 0xE6ECF7, 245);
    } else {
        gfx_fill_round(x, y, bw, bh, S(13), C_ACCENT, 250);
    }
    u32 ink = outgoing ? 0xFFFFFFu : C_INK;
    text_at(&g_font_ui13, x + padx, y + pady + S(11), l1, ink, 245);
    if (l2) text_at(&g_font_ui13, x + padx, y + pady + S(29), l2, ink, 225);
}

static void app_messages_body(int x, int y, int w, int h)
{
    gfx_fill_circle(x + S(18), y + S(18), S(16), C_PURPLE, 90);
    gfx_fill_circle(x + S(18), y + S(18), S(13), 0x7A5CFF, 255);
    text_center(&g_font_ui15sb, x + S(18), y + S(23), "A", 0xFFFFFF, 255);
    text_at(&g_font_ui15sb, x + S(40), y + S(17), "Ava Chen", C_INK, 245);
    text_at(&g_font_ui13, x + S(40), y + S(33), "Active now", C_GREEN, 230);
    gfx_fill_circle(x + S(40) + tw(&g_font_ui13, "Active now") + S(8),
                    y + S(29), S(3), C_GREEN, 255);

    gfx_fill_rect(x, y + S(48), w, 1, C_LINE);

    int top = y + S(58);
    int maxw = w * 66 / 100;
    int left_x = x + S(2);
    int right_x = x + w - maxw - S(2);

    bubble(left_x, top, maxw, "Morning! The new boot animation", 
           "is absolutely gorgeous.", 0);
    bubble(right_x, top + S(58), maxw, "Thank you - compositor, fonts and",
           "animation are all hand written.", 1);
    bubble(left_x, top + S(120), maxw, "The browser opens from the dock,",
           "right? I want to try the Wi-Fi page.", 0);
    bubble(right_x, top + S(178), maxw, "Yep. Every icon down there works.",
           0, 1);

    int iy = y + h - S(42);
    gfx_panel(x, iy, w, S(38), S(19), 0xFFFFFF, 150, 0xDCE4F2, 180);
    text_at(&g_font_ui13, x + S(16), iy + S(24),
            "Message Revertical…", 0x98A3BA, 220);
    int sx = x + w - S(28);
    gfx_fill_circle(sx, iy + S(19), S(14), C_ACCENT, 255);
    line(sx - S(5), iy + S(19), sx + S(4), iy + S(14), 0xFFFFFF, 245);
    line(sx + S(4), iy + S(14), sx + S(2), iy + S(23), 0xFFFFFF, 245);
    line(sx + S(2), iy + S(23), sx - S(2), iy + S(20), 0xFFFFFF, 245);
}

/* ------------------------------------------------------------------------- */
/*  App: Settings                                                             */
/* ------------------------------------------------------------------------- */
static void settings_section(int x, int y, const char *label)
{
    text_tracked(&g_font_ui13, x, y, label, 0x8A94AE, 220, 2);
}

static int wifi_selected;
static char wifi_pass[64];
static char wifi_note[80] = "Select a network, then press Connect.";
static int wifi_focus;

static void settings_note(const char *text)
{
    path_copy(wifi_note, text, sizeof(wifi_note));
}

static void settings_connect(void)
{
    const WifiApInfo *ap = wifi_ap(wifi_selected);
    if (!ap) {
        settings_note("No network selected");
        return;
    }
    char err[80];
    int r = wifi_connect(wifi_selected, wifi_pass, err, sizeof(err));
    if (r == 0) {
        settings_note(wifi_status());
        wifi_focus = 0;
        if (window_visible(APP_BROWSER) &&
            strcmp(browser_url, "about:wifi") == 0)
            browser_navigate(browser_url, wins[APP_BROWSER].w - S(28), 0);
    } else {
        settings_note(err[0] ? err : "Connection failed");
    }
}

static int settings_ap_hit(int x, int y, int w, int mx, int my)
{
    int rows_y = y + S(144);
    int rh = S(22);
    int count = wifi_ap_count();
    for (int i = 0; i < count && i < 5; i++) {
        int yy = rows_y + i * rh;
        if (mx >= x && mx < x + w - S(8) && my >= yy && my < yy + rh)
            return i;
    }
    return -1;
}

static int app_settings_click(int x, int y, int w, int h, int mx, int my)
{
    int right = x + w - S(6);
    int sw_w = S(38);

    if (mx >= right - sw_w && mx < right && my >= y + S(78) &&
        my < y + S(98)) {
        if (wifi_connected())
            wifi_disconnect(), settings_note("Wi-Fi disconnected");
        else
            settings_connect();
        return 1;
    }

    int ap = settings_ap_hit(x, y, w, mx, my);
    if (ap >= 0) {
        if (ap != wifi_selected) {
            wifi_selected = ap;
            wifi_pass[0] = 0;
            const WifiApInfo *info = wifi_ap(ap);
            if (info && info->security == 0)
                settings_note("Open network: press Connect");
            else
                settings_note("Enter the WPA2 passphrase");
        }
        return 1;
    }

    const WifiApInfo *sel = wifi_ap(wifi_selected);
    if (sel && sel->security &&
        mx >= x + S(4) && mx < x + w - S(8) &&
        my >= y + S(258) && my < y + S(284)) {
        wifi_focus = 1;
        return 1;
    }

    if (my >= y + S(288) && my < y + S(316)) {
        if (mx >= x + S(4) && mx < x + S(100)) {
            wifi_focus = 0;
            settings_connect();
            return 1;
        }
        if (mx >= x + S(108) && mx < x + S(212)) {
            wifi_disconnect();
            settings_note("Wi-Fi disconnected");
            return 1;
        }
        if (mx >= x + w - S(126) && mx < x + w - S(4)) {
            if (window_visible(APP_BROWSER)) {
                browser_focus_url = 0;
                browser_navigate("http://revertical.local/",
                                 wins[APP_BROWSER].w - S(28), 1);
                focus_app(APP_BROWSER);
            } else {
                toggle_app(APP_BROWSER);
                browser_navigate("http://revertical.local/",
                                 wins[APP_BROWSER].w - S(28), 1);
            }
            return 1;
        }
    }

    (void)h;
    return 1;
}

static void draw_lock(int x, int y, u32 color)
{
    gfx_fill_round(x - S(4), y - S(1), S(8), S(6), S(2), color, 230);
    gfx_fill_round(x - S(3), y - S(6), S(6), S(7), S(3), color, 90);
    gfx_fill_circle(x, y + S(2), S(1), 0xFFFFFF, 200);
}

static void app_settings_body(int x, int y, int w, int h)
{
    int right = x + w - S(6);
    int sw_w = S(38);
    NetIf *eth = net_wired();
    int eth_up = eth && eth->up && eth->ip;

    settings_section(x + S(4), y + S(14), "CONNECTIVITY");

    gfx_fill_circle(x + S(12), y + S(36), S(5),
                    eth_up ? C_GREEN : 0xB9C5DA, 255);
    text_at(&g_font_ui15sb, x + S(24), y + S(41), "Ethernet", C_INK, 245);
    text_at(&g_font_ui13, x + S(24), y + S(57),
            eth_up ? net_ip_text(eth->ip) : "Link down / no DHCP lease",
            eth_up ? 0x58658A : C_MUTED, 215);
    if (eth_up)
        text_at(&g_font_ui13, right - tw(&g_font_ui13, "DHCP"),
                y + S(41), "DHCP", C_GREEN, 225);

    gfx_fill_rect(x + S(4), y + S(66), w - S(8), 1, C_LINE);

    text_at(&g_font_ui15sb, x + S(4), y + S(92), "Wi-Fi", C_INK, 245);
    text_at(&g_font_ui13, x + S(4), y + S(108),
            wifi_connected() ? wifi_status() : "Not connected",
            wifi_connected() ? C_GREEN : C_MUTED, 220);
    draw_switch(right - sw_w, y + S(78), wifi_connected());

    gfx_fill_rect(x + S(4), y + S(118), w - S(8), 1, C_LINE);
    settings_section(x + S(4), y + S(134), "AVAILABLE NETWORKS");

    int rows_y = y + S(144);
    int rh = S(22);
    int count = wifi_ap_count();
    if (count > 5) count = 5;
    for (int i = 0; i < count; i++) {
        const WifiApInfo *ap = wifi_ap(i);
        int yy = rows_y + i * rh;
        if (i == wifi_selected)
            gfx_fill_round(x + S(2), yy - S(1), w - S(6), rh - S(2),
                           S(7), 0xE8F0FE, 245);
        if (ap->security)
            draw_lock(x + S(14), yy + S(11), C_INK);
        else
            gfx_fill_circle(x + S(14), yy + S(11), S(4), 0x9FB0CC, 160);
        text_fit(&g_font_ui13, x + S(28), yy + S(15), ap->ssid,
                 w - S(150), C_INK, 245);

        char meta[48];
        int n = 0;
        const char *sec = ap->security ? "WPA2" : "Open";
        while (*sec) meta[n++] = *sec++;
        meta[n++] = ' ';
        meta[n++] = ' ';
        if (ap->rssi < 0) meta[n++] = '-';
        int rssi = ap->rssi < 0 ? -ap->rssi : ap->rssi;
        char num[8];
        int k = 0;
        if (rssi == 0) num[k++] = '0';
        while (rssi) { num[k++] = (char)('0' + rssi % 10); rssi /= 10; }
        while (k) meta[n++] = num[--k];
        const char *tail = " dBm";
        while (*tail) meta[n++] = *tail++;
        meta[n] = 0;
        if (i == wifi_selected)
            text_at(&g_font_ui13, right - tw(&g_font_ui13, meta),
                    yy + S(15), meta, C_ACCENT, 235);
        else
            text_at(&g_font_ui13, right - tw(&g_font_ui13, meta),
                    yy + S(15), meta, 0x93A0B8, 205);
    }

    const WifiApInfo *sel = wifi_ap(wifi_selected);
    int py = y + S(258);
    if (sel && sel->security) {
        gfx_panel(x + S(4), py, w - S(8), S(26), S(8),
                  wifi_focus ? 0xFFFFFF : 0xF2F6FD, 255,
                  wifi_focus ? C_ACCENT : C_LINE, 220);
        text_at(&g_font_ui13, x + S(14), py + S(18),
                wifi_pass[0] ? wifi_pass : "WPA2 passphrase",
                wifi_pass[0] ? C_INK : 0x98A3BA, 235);
        if (wifi_focus && ((ui_now / 500) & 1)) {
            int cx = x + S(14) + tw(&g_font_ui13, wifi_pass);
            gfx_fill_rect(cx + 1, py + S(5), 1, S(16), C_ACCENT);
        }
        if (wifi_focus)
            gfx_fill_circle(x + w - S(18), py + S(13), S(4), C_ACCENT, 220);
    } else if (sel) {
        text_at(&g_font_ui13, x + S(6), py + S(18),
                "Open network: no passphrase needed.", C_MUTED, 215);
    }

    int by = y + S(288);
    gfx_fill_round(x + S(4), by, S(96), S(28), S(8), C_ACCENT, 255);
    text_center(&g_font_ui13, x + S(4) + S(48), by + S(19), "Connect",
                0xFFFFFF, 255);
    gfx_fill_round(x + S(108), by, S(104), S(28), S(8), 0xE9EEF8, 255);
    text_center(&g_font_ui13, x + S(108) + S(52), by + S(19), "Disconnect",
                C_INK, 235);
    gfx_fill_round(x + w - S(126), by, S(122), S(28), S(8),
                   0xEAF1FF, 255);
    text_center(&g_font_ui13, x + w - S(126) + S(61), by + S(19),
                "Open browser", C_ACCENT, 245);

    text_fit(&g_font_ui13, x + S(4), y + h - S(6), wifi_note,
             w - S(8), 0x8A94AE, 210);
    (void)h;
}

/* ------------------------------------------------------------------------- */
/*  App: Revertical AI (shell)                                                */
/* ------------------------------------------------------------------------- */
static void app_ai_body(int x, int y, int w, int h)
{
    gfx_radial(x + w - S(40), y + S(20), S(180), 0x9A5CFF, C_PANEL, 34);

    if (a_dock[APP_AI].pixels)
        gfx_blit(&a_dock[APP_AI], x + S(4), y + S(4));

    text_at(&g_font_title19, x + S(4), y + S(88), "Hello, Revertical.",
            C_INK, 250);
    text_at(&g_font_ui13, x + S(4), y + S(110),
            "Your assistant is almost online. The model link arrives",
            0x68748F, 220);
    text_at(&g_font_ui13, x + S(4), y + S(126),
            "with the next build.", 0x68748F, 220);

    static const char *chips[3] = {
        "Summarise my day", "Write some code", "Explain orbital mechanics"
    };
    static const u32 tints[3] = { C_TEAL, C_ACCENT, C_PURPLE };
    for (int i = 0; i < 3; i++) {
        int cy = y + S(146) + i * S(38);
        int cw = tw(&g_font_ui13, chips[i]) + S(38);
        gfx_panel(x + S(4), cy, cw, S(30), S(15), 0xFFFFFF, 120,
                  0xCFDCFA, 170);
        gfx_fill_circle(x + S(18), cy + S(15), S(4), tints[i], 255);
        text_at(&g_font_ui13, x + S(28), cy + S(20), chips[i], C_INK, 230);
    }

    int iy = y + h - S(42);
    gfx_panel(x, iy, w, S(38), S(19), 0xFFFFFF, 170, 0xCFDCFA, 180);
    text_at(&g_font_ui13, x + S(16), iy + S(24), "Ask anything…",
            0x98A3BA, 220);
    int sx = x + w - S(28);
    gfx_fill_circle(sx, iy + S(19), S(14), C_ACCENT, 255);
    line(sx - S(5), iy + S(19), sx + S(4), iy + S(14), 0xFFFFFF, 245);
    line(sx + S(4), iy + S(14), sx + S(2), iy + S(23), 0xFFFFFF, 245);
    line(sx + S(2), iy + S(23), sx - S(2), iy + S(20), 0xFFFFFF, 245);
}

/* ------------------------------------------------------------------------- */
/*  App: Installer                                                            */
/* ------------------------------------------------------------------------- */
static void size_mb_text(char *out, u32 bytes)
{
    u32 mb = bytes / (1024u * 1024u);
    char tmp[12];
    int n = 0;
    if (mb == 0) tmp[n++] = '0';
    while (mb) {
        tmp[n++] = (char)('0' + mb % 10);
        mb /= 10;
    }
    int k = 0;
    while (n) out[k++] = tmp[--n];
    out[k++] = ' ';
    out[k++] = 'M';
    out[k++] = 'B';
    out[k] = 0;
}

static void inst_account_body(int x, int y, int w, int h)
{
    int uy = y + S(78);
    int py = y + S(134);
    int cy = y + S(190);
    int fh = S(36);

    text_at(&g_font_title19, x + S(4), y + S(18), "Create your account",
            C_INK, 250);
    text_at(&g_font_ui13, x + S(4), y + S(36),
            "This account signs you in on the installed system.",
            0x68748F, 220);

    text_tracked(&g_font_ui13, x + S(4), y + S(68), "USERNAME",
                 0x8A94AE, 220, 2);
    draw_field(x + S(4), uy, w - S(8), fh, inst_focus == 0,
               inst_user, "your username", 0);

    text_tracked(&g_font_ui13, x + S(4), y + S(124), "PASSWORD",
                 0x8A94AE, 220, 2);
    draw_field(x + S(4), py, w - S(8), fh, inst_focus == 1,
               inst_pass, "4 to 40 characters", 1);

    text_tracked(&g_font_ui13, x + S(4), y + S(180), "CONFIRM PASSWORD",
                 0x8A94AE, 220, 2);
    draw_field(x + S(4), cy, w - S(8), fh, inst_focus == 2,
               inst_confirm, "type it again", 1);

    text_at(&g_font_ui13, x + S(4), y + S(238),
            "2-20 lowercase characters, starting with a letter.",
            0x58658A, 210);
    text_at(&g_font_ui13, x + S(4), y + S(254),
            "The password is stored as PBKDF2-HMAC-SHA1 in /System/users.",
            0x58658A, 210);

    if (inst_error[0])
        text_fit(&g_font_ui13, x + S(4), y + S(282), inst_error,
                 w - S(8), 0xB24343, 240);

    int by = y + h - S(44);
    int hot = point_in(x, by, w, S(38), mouse_x, mouse_y);
    gfx_shadow_round(x, by + S(3), w, S(38), S(13), S(8), 0x050B18, 60);
    gfx_fill_round(x, by, w, S(38), S(12),
                   hot ? 0x4D82FFu : C_ACCENT, 255);
    text_center(&g_font_ui15sb, x + w / 2, by + S(25),
                "Continue to disk", 0xFFFFFF, 250);
}

static int inst_account_commit(void)
{
    inst_error[0] = 0;
    if (accounts_validate_name(inst_user, inst_error,
                               sizeof(inst_error)) != 0) {
        inst_focus = 0;
        return 0;
    }
    if (accounts_validate_pass(inst_pass, inst_error,
                               sizeof(inst_error)) != 0) {
        inst_focus = 1;
        return 0;
    }
    if (strcmp(inst_pass, inst_confirm) != 0) {
        path_copy(inst_error, "Passwords do not match", sizeof(inst_error));
        inst_focus = 2;
        return 0;
    }

    installer_set_account(inst_user, inst_pass);
    if (!installer_account_ready()) {
        path_copy(inst_error, "That account could not be used",
                  sizeof(inst_error));
        return 0;
    }

    inst_error[0] = 0;
    inst_confirm[0] = 0;
    inst_stage = 1;
    inst_focus = 0;
    return 1;
}

static int inst_account_click(int x, int y, int w, int h, int mx, int my)
{
    int uy = y + S(78);
    int py = y + S(134);
    int cy = y + S(190);
    int fh = S(36);

    if (point_in(x + S(4), uy, w - S(8), fh, mx, my)) {
        inst_focus = 0;
        return 1;
    }
    if (point_in(x + S(4), py, w - S(8), fh, mx, my)) {
        inst_focus = 1;
        return 1;
    }
    if (point_in(x + S(4), cy, w - S(8), fh, mx, my)) {
        inst_focus = 2;
        return 1;
    }
    int by = y + h - S(44);
    if (point_in(x, by, w, S(38), mx, my))
        inst_account_commit();
    return 1;
}

static void inst_account_key(int c)
{
    if (c == KEY_TAB) {
        inst_focus = (inst_focus + 1) % 3;
        return;
    }
    if (c == KEY_ENTER) {
        if (inst_focus < 2)
            inst_focus++;
        else
            inst_account_commit();
        return;
    }
    if (c == KEY_ESC) {
        inst_error[0] = 0;
        return;
    }
    if (c == KEY_BACK) {
        char *buf = inst_focus == 0 ? inst_user
                   : inst_focus == 1 ? inst_pass : inst_confirm;
        int n = (int)strlen(buf);
        if (n > 0) buf[n - 1] = 0;
        inst_error[0] = 0;
        return;
    }
    if (c < 0x20 || c >= 0x7F)
        return;

    inst_error[0] = 0;
    char *buf = inst_focus == 0 ? inst_user
               : inst_focus == 1 ? inst_pass : inst_confirm;
    int max = inst_focus == 0 ? USER_NAME_MAX : USER_PASS_MAX;
    if (inst_focus == 0) {
        if (c == ' ')
            return;
        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
    }
    int n = (int)strlen(buf);
    if (n < max) {
        buf[n] = (char)c;
        buf[n + 1] = 0;
    }
}

static void app_installer_body(int x, int y, int w, int h)
{
    if (inst_stage == 0) {
        inst_account_body(x, y, w, h);
        return;
    }

    int count = installer_drive_count();
    int rh = S(52);
    int ry = y + S(56);
    int phase = installer.phase;

    text_at(&g_font_title19, x + S(4), y + S(18), "Install ReverticalOS",
            C_INK, 250);
    char subtitle[100];
    int sn = 0;
    const char *who = installer.account_user[0] ? installer.account_user
                                                 : "no account";
    const char *pre = "Account: ";
    while (*pre && sn < 99) subtitle[sn++] = *pre++;
    for (int i = 0; who[i] && sn < 99; i++) subtitle[sn++] = who[i];
    const char *tail = installer.boot_dev < 0
        ? "  ·  live CD, choose a target disk"
        : "  ·  choose a target disk";
    for (int i = 0; tail[i] && sn < 99; i++) subtitle[sn++] = tail[i];
    subtitle[sn] = 0;
    text_fit(&g_font_ui13, x + S(4), y + S(36), subtitle,
             w - S(120), 0x68748F, 220);

    const char *change = "Change account";
    int change_w = tw(&g_font_ui13, change) + S(14);
    int change_x = x + w - change_w;
    int change_hot = point_in(change_x, y, change_w, S(24),
                              mouse_x, mouse_y);
    if (change_hot)
        gfx_fill_round(change_x, y - S(2), change_w, S(24), S(8),
                       0xE8F0FE, 230);
    text_at(&g_font_ui13, change_x + S(7), y + S(15), change,
            C_ACCENT, 235);

    for (int i = 0; i < count && i < 4; i++) {
        int yy = ry + i * rh;
        int sel = installer_target() == i;
        gfx_panel(x, yy, w, rh - S(6), S(11),
                  sel ? 0xEFF4FF : 0xFFFFFF, sel ? 235 : 150,
                  sel ? C_ACCENT : 0xDCE4F2, sel ? 210 : 150);
        int cy = yy + (rh - S(6)) / 2;
        gfx_fill_circle(x + S(18), cy, S(8),
                        sel ? C_ACCENT : 0xE2E8F4, 255);
        if (sel)
            gfx_fill_circle(x + S(18), cy, S(3), 0xFFFFFF, 255);

        const char *model = disk_model(i);
        if (!model[0])
            model = "ATA disk";
        text_fit(&g_font_ui15sb, x + S(38), yy + S(23), model,
                 w - S(150), C_INK, 245);

        char cap[24];
        size_mb_text(cap, disk_size_bytes(i));
        text_at(&g_font_ui13, x + S(38), yy + S(39), cap, C_MUTED, 210);

        if (i == installer.boot_dev) {
            int cw = tw(&g_font_ui13, "RUNNING") + S(16);
            gfx_fill_round(x + w - cw - S(10),
                           yy + (rh - S(6) - S(20)) / 2, cw, S(20), S(10),
                           0xE8F0FE, 235);
            text_center(&g_font_ui13, x + w - S(10) - cw / 2,
                        yy + (rh - S(6)) / 2 + S(4), "RUNNING", C_ACCENT, 235);
        }
    }

    if (count == 0)
        text_at(&g_font_ui13, x + S(4), ry + S(22),
                "No ATA disks were detected.", 0xB24343, 220);

    int sy = ry + 4 * rh + S(2);

    if (phase != INSTALL_IDLE) {
        text_at(&g_font_ui13, x + S(4), sy + S(10), installer.status,
                C_INK, 240);

        char pct[8];
        int p = installer.percent;
        int k = 0;
        if (p >= 100) {
            pct[k++] = '1'; pct[k++] = '0'; pct[k++] = '0';
        } else if (p >= 10) {
            pct[k++] = (char)('0' + p / 10);
            pct[k++] = (char)('0' + p % 10);
        } else {
            pct[k++] = (char)('0' + p);
        }
        pct[k++] = '%';
        pct[k] = 0;
        text_at(&g_font_ui13, x + w - tw(&g_font_ui13, pct), sy + S(10),
                pct, C_MUTED, 220);

        gfx_fill_round(x + S(4), sy + S(20), w - S(8), S(6), S(3),
                       0xD8E1F0, 255);
        if (p > 0)
            gfx_fill_round(x + S(4), sy + S(20), (w - S(8)) * p / 100,
                           S(6), S(3), C_ACCENT, 255);

        int lines = installer.log_count;
        int first = lines > 3 ? lines - 3 : 0;
        for (int i = first; i < lines; i++) {
            int ly = sy + S(42) + (i - first) * S(16);
            gfx_fill_circle(x + S(8), ly - S(4), S(2), 0x9BB0CC, 220);
            text_fit(&g_font_ui13, x + S(16), ly, installer.log[i],
                     w - S(20), 0x58658A, 215);
        }
    } else if (count > 0) {
        text_at(&g_font_ui13, x + S(4), sy + S(22),
                "The installer writes an MBR, the bootloader, the kernel and",
                0x58658A, 215);
        text_at(&g_font_ui13, x + S(4), sy + S(38),
                "a fresh RVFS volume - then marks the disk as installed.",
                0x58658A, 215);
    }

    int by = y + h - S(44);
    u32 fill = C_ACCENT;
    const char *label = "Erase and install";
    if (phase == INSTALL_RUNNING) {
        fill = 0xC9D4E6;
        label = "Installing - please wait";
    } else if (phase == INSTALL_DONE) {
        fill = C_GREEN;
        if (installer.boot_dev < 0)
            label = "Restart live CD";
        else
            label = installer.target == installer.boot_dev ? "Restart now"
                                                           : "Restart live system";
    } else if (phase == INSTALL_FAILED) {
        label = "Try again";
    } else if (inst_wipe_confirm) {
        fill = C_RED;
        label = "Click again to erase the disk";
    }

    if (phase == INSTALL_IDLE && count > 0)
        text_at(&g_font_ui13, x + S(4), by - S(10),
                "All partitions on the selected disk will be destroyed.",
                0xB24343, 215);

    if (phase == INSTALL_DONE) {
        const char *model = disk_model(installer.target);
        if (!model[0])
            model = "the selected disk";
        const char *prefix = "Installed to ";
        int hx = x + S(4);
        int hy = by - S(34);
        text_at(&g_font_ui13, hx, hy, prefix, C_MUTED, 220);
        text_at(&g_font_ui13, hx + tw(&g_font_ui13, prefix), hy, model,
                C_INK, 240);

        if (installer.boot_dev < 0) {
            text_at(&g_font_ui13, hx, hy + S(16),
                    "Installed. Eject the CD and boot this disk on its own.",
                    0x58658A, 215);
        } else if (installer.target != installer.boot_dev) {
            text_at(&g_font_ui13, hx, hy + S(16),
                    "That disk is not the boot disk - start it from the VM's "
                    "boot order.", 0x58658A, 215);
        } else {
            text_at(&g_font_ui13, hx, hy + S(16),
                    "Restarting this disk boots the installed system.",
                    0x58658A, 215);
        }
    }

    gfx_shadow_round(x, by + S(3), w, S(38), S(13), S(8), 0x050B18, 60);
    gfx_fill_round(x, by, w, S(38), S(12), fill, 255);
    text_center(&g_font_ui15sb, x + w / 2, by + S(25), label,
                0xFFFFFF, 250);
}

static int app_installer_click(int x, int y, int w, int h, int mx, int my)
{
    if (inst_stage == 0)
        return inst_account_click(x, y, w, h, mx, my);

    if (installer.phase == INSTALL_RUNNING)
        return 1;

    const char *change = "Change account";
    int change_w = tw(&g_font_ui13, change) + S(14);
    if (point_in(x + w - change_w, y, change_w, S(24), mx, my)) {
        inst_stage = 0;
        inst_focus = 0;
        inst_error[0] = 0;
        return 1;
    }

    int count = installer_drive_count();
    int rh = S(52);
    int ry = y + S(56);
    for (int i = 0; i < count && i < 4; i++) {
        int yy = ry + i * rh;
        if (point_in(x, yy, w, rh - S(6), mx, my)) {
            installer_select(i);
            inst_wipe_confirm = 0;
            return 1;
        }
    }

    int by = y + h - S(44);
    if (point_in(x, by, w, S(38), mx, my)) {
        if (installer.phase == INSTALL_DONE) {
            start_power(1);
            return 1;
        }
        if (installer.phase == INSTALL_FAILED) {
            installer_select(installer.target);
            return 1;
        }
        if (installer.phase == INSTALL_IDLE && count > 0) {
            if (!inst_wipe_confirm) {
                inst_wipe_confirm = 1;
            } else {
                inst_wipe_confirm = 0;
                installer_start(installer.target);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/*  Live-environment desktop shortcut                                         */
/* ------------------------------------------------------------------------- */
static void shortcut_rect(int *x, int *y, int *w, int *h)
{
    *w = S(296);
    *h = S(78);
    *x = S(22);
    *y = S(18);
}

static void draw_shortcut(void)
{
    if (!live_env)
        return;
    int x, y, w, h;
    shortcut_rect(&x, &y, &w, &h);

    if (shortcut_hover)
        gfx_shadow_round(x, y + S(4), w, h, S(18), S(12), 0x050B18, 70);
    glass(x, y, w, h, S(18), 1);
    if (shortcut_hover)
        gfx_fill_round(x, y, w, h, S(18), 0xFFFFFF, 70);

    if (a_dock[APP_INSTALLER].pixels)
        gfx_blit(&a_dock[APP_INSTALLER], x + S(14), y + (h - S(48)) / 2);

    text_at(&g_font_ui15sb, x + S(74), y + S(31), "Install ReverticalOS",
            C_INK, 250);
    text_at(&g_font_ui13, x + S(74), y + S(50),
            "Live system  ·  choose a target disk", 0x68748F, 215);

    int cx = x + w - S(20);
    int cy = y + h / 2;
    line(cx - S(4), cy - S(4), cx, cy, 0x93A0B8, 220);
    line(cx, cy, cx - S(4), cy + S(4), 0x93A0B8, 220);
}

/* ------------------------------------------------------------------------- */
/*  Window chrome                                                             */
/* ------------------------------------------------------------------------- */
static void draw_close_button(int x, int y, int red)
{
    u32 fill = red ? 0xFFDDE0u : 0xE9EEF8u;
    gfx_fill_circle(x, y, S(10), fill, 255);
    line(x - S(3), y - S(3), x + S(3), y + S(3), red ? C_RED : 0x93A0B8, 255);
    line(x - S(3), y + S(3), x + S(3), y - S(3), red ? C_RED : 0x93A0B8, 255);
}

static void draw_min_button(int x, int y, int hot)
{
    gfx_fill_circle(x, y, S(10), hot ? 0xE2E9F6u : 0xEDF1F9u, 255);
    gfx_fill_round(x - S(4), y - S(1), S(9), S(2), S(1), 0x93A0B8, 255);
}

static void draw_window(int app)
{
    Window *w = &wins[app];
    if (!window_visible(app)) return;

    int x = w->x, y = w->y;
    u64 age = ui_now - w->open_time;
    if (age < 190)
        y += (int)((190 - age) * (u64)S(16) / 190);
    if (y + w->h > screen_h) y = screen_h - w->h;

    gfx_shadow_round(x, y + S(7), w->w, w->h, S(17), S(18), 0x050B18, 95);
    gfx_panel(x, y, w->w, w->h, S(17), C_PANEL, 247, 0xFFFFFF, 200);

    static const u32 tints[APP_COUNT] = {
        C_TEAL, C_ACCENT, C_PURPLE, 0x6E7E9Eu, C_PINK, 0x2F9B8Fu, 0x2FBF71u
    };
    draw_badge(x + S(24), y + S(26), tints[app], S(6));
    text_at(&g_font_ui15sb, x + S(40), y + S(31), app_names[app],
            C_INK, 250);

    draw_min_button(x + w->w - S(60), y + S(24), min_hover == app);
    draw_close_button(x + w->w - S(30), y + S(24), close_hover == app);

    gfx_fill_rect(x + S(16), y + S(50), w->w - S(32), 1, C_LINE);

    int cx = x + S(14);
    int cy = y + S(58);
    int cw = w->w - S(28);
    int ch = w->h - S(72);

    gfx_set_clip(cx, cy, cw, ch);
    switch (app) {
    case APP_BROWSER:  app_browser_body(cx, cy, cw, ch); break;
    case APP_FILES:    app_files_body(cx, cy, cw, ch); break;
    case APP_MESSAGES: app_messages_body(cx, cy, cw, ch); break;
    case APP_SETTINGS: app_settings_body(cx, cy, cw, ch); break;
    case APP_AI:       app_ai_body(cx, cy, cw, ch); break;
    case APP_INSTALLER: app_installer_body(cx, cy, cw, ch); break;
    case APP_CONSOLE:  app_console_body(cx, cy, cw, ch); break;
    }
    gfx_reset_clip();
}

/* ------------------------------------------------------------------------- */
/*  Desktop furniture                                                         */
/* ------------------------------------------------------------------------- */
static void draw_tooltip(const char *label, int cx, int above_y);

static void draw_pill(void)
{
    glass(L.pill_x, L.pill_y, L.pill_w, L.pill_h, S(17), 1);
    int start_right = L.text_x + tw(&g_font_title19, "ReverticalOS") + S(9);
    int start_hot = point_in(L.pill_x, L.pill_y, start_right - L.pill_x,
                             L.pill_h, mouse_x, mouse_y);
    if (start_open || start_hot) {
        gfx_fill_round(L.pill_x + S(5), L.pill_y + S(5),
                       start_right - L.pill_x - S(5), L.pill_h - S(10),
                       S(13), 0xE8F0FE, 230);
    }
    if (a_logo_sm.pixels)
        gfx_blit(&a_logo_sm, L.logo_x, L.logo_y);
    text_at(&g_font_title19, L.text_x, L.text_baseline, "ReverticalOS",
            C_INK, 250);
    if (start_hot && !start_open)
        draw_tooltip("Start menu", (L.pill_x + start_right) / 2, L.pill_y);

    static const char *tips[4] = {
        "Search", "Switch windows", "Files", "Notifications"
    };
    for (int i = 0; i < 4; i++) {
        int ix = L.nav_x[i];
        int iy = L.nav_y;
        if (nav_hover == i) {
            gfx_fill_round(ix - S(6), iy - S(6), L.nav_size + S(12),
                           L.nav_size + S(12), S(10), 0xFFFFFF, 105);
        }
        if (a_nav[i].pixels)
            gfx_blit(&a_nav[i], ix, iy);
        if (nav_hover == i)
            draw_tooltip(tips[i], ix + L.nav_size / 2, L.pill_y);
    }

    if (notif_unread() > 0) {
        int bx = L.nav_x[3] + L.nav_size - S(2);
        int by = L.nav_y + S(3);
        gfx_fill_circle(bx, by, S(4), C_PANEL, 240);
        gfx_fill_circle(bx, by, S(3), C_RED, 255);
    }
}

static void draw_tooltip(const char *label, int cx, int above_y)
{
    int w = tw(&g_font_ui13, label) + S(22);
    int h = S(25);
    int x = cx - w / 2;
    x = iclamp(x, S(6), screen_w - w - S(6));
    int y = above_y - h - S(10);
    gfx_shadow_round(x, y, w, h, h / 2, S(8), 0x050B18, 70);
    gfx_fill_round(x, y, w, h, h / 2, 0x101C38, 238);
    text_center(&g_font_ui13, x + w / 2, y + h / 2 + S(5), label,
                0xFFFFFF, 240);
}

static void draw_dock(void)
{
    glass(L.dock_x, L.dock_y, L.dock_w, L.dock_h, S(18), 1);

    static const char *tips[APP_COUNT] = {
        "Browser", "Files", "Messages", "Settings", "Revertical AI", "Installer",
        "Program Output"
    };
    for (int i = 0; i < DOCK_APPS; i++) {
        int ix = L.dock_ix[i];
        int iy = L.dock_iy;
        int lift = 0;
        if (dock_hover == i) {
            lift = S(3);
            gfx_fill_round(ix - S(5), iy - S(5) - lift, L.dock_size + S(10),
                           L.dock_size + S(10), S(12), 0xFFFFFF, 120);
        }
        if (a_dock[i].pixels)
            gfx_blit(&a_dock[i], ix, iy - lift);
        if (wins[i].open) {
            int dy2 = L.dock_y + L.dock_h - S(7);
            if (wins[i].minimized)
                gfx_fill_circle(ix + L.dock_size / 2, dy2, S(2),
                                0x93A0B8, 200);
            else
                gfx_fill_circle(ix + L.dock_size / 2, dy2, S(2),
                                C_ACCENT, 230);
        }
        if (dock_hover == i)
            draw_tooltip(tips[i], ix + L.dock_size / 2, L.dock_y);
    }
    (void)tips;
}

static void draw_card(void)
{
    glass(L.card_x, L.card_y, L.card_w, L.card_h, S(21), 1);

    gfx_panel(L.top_x, L.top_y, L.top_w, L.top_h, S(13),
              0xFFFFFF, 125, 0xFFFFFF, 160);
    text_center(&g_font_title19, L.top_x + L.top_w / 2,
                L.top_y + S(26), clock_text, C_INK, 250);
    text_center(&g_font_ui13, L.top_x + L.top_w / 2,
                L.top_y + S(42), date_text, 0x596686, 230);

    rainbow_border(L.bot_x, L.bot_y, L.bot_w, L.bot_h, S(15));
    gfx_fill_round(L.bot_x + S(2), L.bot_y + S(2), L.bot_w - S(4),
                   L.bot_h - S(4), S(13), 0xFFFFFF, 132);
    text_at(&g_font_ui15sb, L.bot_x + S(14), L.bot_y + L.bot_h / 2 + S(5),
            "84%", C_INK, 245);

    if (a_bars.pixels)
        gfx_blit(&a_bars, L.bot_x + L.bot_w - S(13) - (int)a_bars.width,
                 L.bot_y + (L.bot_h - (int)a_bars.height) / 2);
    if (a_wifi.pixels)
        gfx_blit(&a_wifi, L.bot_x + L.bot_w - S(25) - (int)a_wifi.width
                 - (int)a_bars.width,
                 L.bot_y + (L.bot_h - (int)a_wifi.height) / 2);
}

/* ------------------------------------------------------------------------- */
/*  Notifications                                                             */
/* ------------------------------------------------------------------------- */
static void notif_push(const char *text, u32 color)
{
    if (notif_count >= NOTIF_MAX) {
        for (int i = 1; i < NOTIF_MAX; i++)
            notifs[i - 1] = notifs[i];
        notif_count = NOTIF_MAX - 1;
    }
    Notif *n = &notifs[notif_count++];
    int i = 0;
    while (text[i] && i < (int)sizeof(n->text) - 1) {
        n->text[i] = text[i];
        i++;
    }
    n->text[i] = 0;
    n->color = color;
    n->time = ui_now;
    n->read = 0;
}

static int notif_unread(void)
{
    int n = 0;
    for (int i = 0; i < notif_count; i++)
        if (!notifs[i].read) n++;
    return n;
}

static int toast_active(void)
{
    for (int i = 0; i < notif_count; i++) {
        if (ui_now - notifs[i].time < 5200)
            return 1;
    }
    return 0;
}

static void draw_toasts(void)
{
    int shown = 0;
    int tw_ = S(306);
    int th = S(52);
    int x = screen_w - tw_ - S(18);
    for (int i = notif_count - 1; i >= 0 && shown < 3; i--) {
        Notif *n = &notifs[i];
        if (ui_now - n->time >= 5200)
            continue;
        int y = S(18) + shown * (th + S(9));
        gfx_shadow_round(x, y + S(3), tw_, th, S(14), S(10), 0x050B18, 70);
        gfx_panel(x, y, tw_, th, S(14), C_PANEL, 242, 0xFFFFFF, 190);
        gfx_fill_round(x, y + S(8), S(4), th - S(16), S(2), n->color, 240);
        text_at(&g_font_ui15sb, x + S(16), y + S(22), "ReverticalOS",
                C_INK, 240);
        text_fit(&g_font_ui13, x + S(16), y + S(39), n->text,
                 tw_ - S(28), 0x4A5670, 230);
        shown++;
    }
}

static void notif_panel_rect(int *x, int *y, int *w, int *h)
{
    int row = S(34);
    int count = notif_count < 1 ? 1 : notif_count;
    int ww = S(324);
    int hh = S(44) + count * row + S(38);
    int xx = L.pill_x;
    int yy = L.pill_y - hh - S(12);
    if (yy < S(8)) {
        yy = S(8);
        hh = L.pill_y - S(20);
    }
    *x = xx;
    *y = yy;
    *w = ww;
    *h = hh;
}

static void draw_notifications(void)
{
    if (!notif_open) return;
    int x, y, w, h;
    notif_panel_rect(&x, &y, &w, &h);
    gfx_shadow_round(x, y + S(4), w, h, S(18), S(14), 0x050B18, 80);
    gfx_panel(x, y, w, h, S(18), C_PANEL, 242, 0xFFFFFF, 185);
    text_at(&g_font_ui15sb, x + S(16), y + S(28), "Notifications",
            C_INK, 250);

    int unread = notif_unread();
    char badge[10];
    int n = 0;
    if (unread > 9) {
        badge[n++] = '9';
        badge[n++] = '+';
    } else {
        badge[n++] = (char)('0' + unread);
    }
    badge[n++] = ' ';
    badge[n++] = 'N';
    badge[n++] = 'E';
    badge[n++] = 'W';
    badge[n] = 0;
    text_tracked(&g_font_ui13, x + w - S(16) - tw(&g_font_ui13, badge),
                 y + S(28), badge, unread ? C_ACCENT : 0x8A94AE,
                 unread ? 220 : 160, 1);

    int row = S(34);
    int ry = y + S(44);
    if (notif_count == 0) {
        text_at(&g_font_ui13, x + S(18), ry + S(21),
                "You're all caught up.", C_MUTED, 210);
    } else {
        for (int i = 0; i < notif_count; i++) {
            Notif *nt = &notifs[i];
            int yy = ry + i * row;
            if (i == notif_hover) {
                gfx_fill_round(x + S(10), yy, w - S(20), row - S(3),
                               S(9), 0xFFFFFF, 150);
            }
            gfx_fill_circle(x + S(23), yy + S(15), S(4),
                            nt->read ? 0xC3CCDD : nt->color,
                            nt->read ? 170 : 255);
            text_fit(&g_font_ui13, x + S(36), yy + S(19), nt->text,
                     w - S(54), nt->read ? 0x7C87A0 : C_INK,
                     nt->read ? 190 : 242);
        }
    }

    if (notif_count > 0) {
        int cy = y + h - S(30);
        text_at(&g_font_ui13, x + S(16), cy + S(12),
                "Click an item to mark it read", 0x8A94AE, 190);
        const char *clear = "Clear all";
        int cw = tw(&g_font_ui13, clear) + S(18);
        int cx = x + w - cw - S(10);
        int hot = point_in(cx, cy - S(2), cw, S(22), mouse_x, mouse_y);
        if (hot)
            gfx_fill_round(cx, cy - S(2), cw, S(22), S(8),
                           0xE8F0FE, 240);
        text_center(&g_font_ui13, cx + cw / 2, cy + S(12), clear,
                    C_ACCENT, 235);
    }
}

/* ------------------------------------------------------------------------- */
/*  Cursor                                                                    */
/* ------------------------------------------------------------------------- */
static void draw_cursor(void)
{
    if (!a_cursor.pixels) return;
    gfx_blit(&a_cursor, mouse_x, mouse_y);
}

/* ------------------------------------------------------------------------- */
/*  Start menu                                                                */
/* ------------------------------------------------------------------------- */
static int start_row_h(void)
{
    return S(31);
}

static void start_rect(int *x, int *y, int *w, int *h)
{
    int row = start_row_h();
    int ww = S(292);
    int hh = S(10) + S(38) + APP_COUNT * row + S(9) +
             START_ACTIONS * row + S(8);
    int xx = L.pill_x;
    int yy = L.pill_y - hh - S(10);
    if (yy < S(8))
        yy = S(8);
    *x = xx;
    *y = yy;
    *w = ww;
    *h = hh;
}

static int start_row_y(int menu_y, int index)
{
    int row = start_row_h();
    if (index < APP_COUNT)
        return menu_y + S(10) + S(38) + index * row;
    return menu_y + S(10) + S(38) + APP_COUNT * row + S(9) +
           (index - APP_COUNT) * row;
}

static int start_button_hit(int mx, int my)
{
    int right = L.text_x + tw(&g_font_title19, "ReverticalOS") + S(10);
    return point_in(L.pill_x, L.pill_y, right - L.pill_x, L.pill_h, mx, my);
}

static int start_row_hit(int mx, int my)
{
    int x, y, w, h;
    start_rect(&x, &y, &w, &h);
    if (!point_in(x, y, w, h, mx, my))
        return -1;
    int row = start_row_h();
    for (int i = 0; i < START_ENTRIES; i++) {
        int ry = start_row_y(y, i);
        if (point_in(x + S(8), ry, w - S(16), row - S(3), mx, my))
            return i;
    }
    return -1;
}

static void start_icon_power(int cx, int cy, u32 color)
{
    for (int i = 0; i < 11; i++) {
        int a = (35 + i * 29) * 256 / 360;
        int px = cx + cos_i(a) * S(6) / 65536;
        int py = cy + sin_i(a) * S(6) / 65536;
        gfx_fill_circle(px, py, S(1), color, 240);
    }
    gfx_fill_round(cx - S(1), cy - S(8), S(2), S(8), S(1), color, 240);
}

static void start_icon_restart(int cx, int cy, u32 color)
{
    for (int i = 0; i < 12; i++) {
        int a = i * 30 * 256 / 360;
        int px = cx + cos_i(a) * S(6) / 65536;
        int py = cy + sin_i(a) * S(6) / 65536;
        gfx_fill_circle(px, py, S(1), color, 235);
    }
    line(cx + S(3), cy - S(8), cx + S(8), cy - S(6), color, 240);
    line(cx + S(8), cy - S(6), cx + S(6), cy - S(1), color, 240);
}

static void draw_start_menu(void)
{
    if (!start_open)
        return;
    int x, y, w, h;
    start_rect(&x, &y, &w, &h);
    int row = start_row_h();

    gfx_shadow_round(x, y + S(6), w, h, S(19), S(18), 0x050B18, 85);
    gfx_panel(x, y, w, h, S(19), C_PANEL, 246, 0xFFFFFF, 195);

    const char *who = accounts_current_valid() ? accounts_current()
                                               : "Live session";
    text_tracked(&g_font_ui13, x + S(16), y + S(24), "REVERTICALOS",
                 0x8A94AE, 220, 2);
    text_fit(&g_font_ui13, x + w - S(16) - tw(&g_font_ui13, who),
             y + S(24), who, S(120), C_INK, 230);
    gfx_fill_rect(x + S(12), y + S(32), w - S(24), 1, C_LINE);

    static const u32 tints[APP_COUNT] = {
        C_TEAL, C_ACCENT, C_PURPLE, 0x6E7E9Eu, C_PINK, 0x2F9B8Fu, 0x2FBF71u
    };

    for (int i = 0; i < START_ENTRIES; i++) {
        int ry = start_row_y(y, i);
        int hot = (i == start_hover) ||
                  (start_hover < 0 && i == start_sel);

        if (i == APP_COUNT)
            gfx_fill_rect(x + S(12), ry - S(5), w - S(24), 1, C_LINE);

        if (hot)
            gfx_fill_round(x + S(8), ry, w - S(16), row - S(3), S(9),
                           0xE8F0FE, 235);

        int cy = ry + (row - S(3)) / 2;
        if (i < APP_COUNT) {
            draw_badge(x + S(22), cy, tints[i], S(4));
            text_at(&g_font_ui13, x + S(38), cy + S(5), app_names[i],
                    C_INK, 245);
        } else {
            int a = i - APP_COUNT;
            if (a == 0) {
                draw_lock(x + S(22), cy, C_INK);
                text_at(&g_font_ui13, x + S(38), cy + S(5), "Lock",
                        C_INK, 245);
            } else if (a == 1) {
                start_icon_power(x + S(22), cy, C_RED);
                text_at(&g_font_ui13, x + S(38), cy + S(5), "Shut down",
                        C_INK, 245);
            } else {
                start_icon_restart(x + S(22), cy, C_INK);
                text_at(&g_font_ui13, x + S(38), cy + S(5), "Restart",
                        C_INK, 245);
            }
        }
    }
}

static void start_power(int restart)
{
    start_open = 0;
    notif_open = 0;
    inst_wipe_confirm = 0;
    drag_app = -1;
    screen_mode = SCREEN_POWER;
    power_action = restart;
    power_at = ui_now;
}

static void start_activate(int index)
{
    if (index < 0 || index >= START_ENTRIES)
        return;

    start_open = 0;
    if (index < APP_COUNT) {
        toggle_app(index);
        return;
    }

    int action = index - APP_COUNT;
    if (installer_installing()) {
        notif_push("Installation is still running", C_RED);
        return;
    }
    if (action == 0) {
        if (!accounts_current_valid()) {
            notif_push("Install ReverticalOS to create an account", C_RED);
            return;
        }
        screen_mode = SCREEN_LOCK;
        auth_focus = 0;
        auth_error[0] = 0;
        login_pass[0] = 0;
        return;
    }
    start_power(action == 2);
}

/* ------------------------------------------------------------------------- */
/*  Login, lock and power screens                                             */
/* ------------------------------------------------------------------------- */
typedef struct {
    int x, y, w, h;
    int ux, uy, uw, uh;
    int px, py, pw, ph;
    int bx, by, bw, bh;
} AuthLayout;

static void auth_layout(AuthLayout *a, int lock)
{
    a->w = S(430);
    a->h = lock ? S(336) : S(388);
    a->x = (screen_w - a->w) / 2;
    a->y = (screen_h - a->h) / 2;

    a->ux = a->x + S(38);
    a->uw = a->w - S(76);
    a->uh = S(36);
    a->px = a->ux;
    a->pw = a->uw;
    a->ph = S(36);
    a->bx = a->ux;
    a->bw = a->uw;
    a->bh = S(38);

    if (lock) {
        a->uy = 0;
        a->py = a->y + S(162);
        a->by = a->y + S(232);
        a->bh = S(36);
    } else {
        a->uy = a->y + S(116);
        a->py = a->y + S(176);
        a->by = a->y + S(248);
    }
}

static void draw_field(int x, int y, int w, int h, int focused,
                       const char *text, const char *placeholder,
                       int password)
{
    u32 fill = focused ? 0xFFFFFFu : 0xF6F9FEu;
    gfx_panel(x, y, w, h, S(9), fill, 255,
              focused ? C_ACCENT : 0xDCE4F2, focused ? 220 : 170);

    char shown[USER_PASS_MAX + 1];
    const char *s = text;
    if (password) {
        int n = (int)strlen(text);
        if (n > USER_PASS_MAX) n = USER_PASS_MAX;
        for (int i = 0; i < n; i++)
            shown[i] = '*';
        shown[n] = 0;
        s = shown;
    }

    if (!s[0]) {
        text_at(&g_font_ui13, x + S(12), y + h / 2 + S(5),
                placeholder, 0x9AA5BB, 210);
    } else {
        text_at(&g_font_ui13, x + S(12), y + h / 2 + S(5), s,
                C_INK, 242);
    }

    if (focused && ((ui_now / 500) & 1)) {
        int cx = x + S(12) + tw(&g_font_ui13, s);
        if (cx > x + w - S(8))
            cx = x + w - S(8);
        gfx_fill_rect(cx + 1, y + S(9), 1, h - S(18), C_ACCENT);
    }
}

static void auth_subtitle(const char *text, int cx, int y)
{
    text_center(&g_font_ui13, cx, y, text, 0x68748F, 225);
}

static void auth_error_text(int cx, int y)
{
    if (!auth_error[0])
        return;
    gfx_fill_round(cx - S(190), y - S(15), S(380), S(24), S(7),
                   0xFFECEE, 235);
    text_center(&g_font_ui13, cx, y + S(1), auth_error, 0xB24343, 240);
}

static void auth_button(int x, int y, int w, int h, const char *label)
{
    int hot = point_in(x, y, w, h, mouse_x, mouse_y);
    gfx_shadow_round(x, y + S(3), w, h, S(12), S(8), 0x050B18, 55);
    gfx_fill_round(x, y, w, h, S(12),
                   hot ? 0x4D82FFu : C_ACCENT, 255);
    text_center(&g_font_ui15sb, x + w / 2, y + h / 2 + S(6), label,
                0xFFFFFF, 250);
}

static void draw_login_screen(void)
{
    gfx_copy_wallpaper();
    gfx_fill_rect_a(0, 0, screen_w, screen_h, 0x07101F, 125);

    AuthLayout a;
    auth_layout(&a, 0);
    int cx = a.x + a.w / 2;

    gfx_shadow_round(a.x, a.y + S(8), a.w, a.h, S(22), S(22),
                     0x030814, 110);
    gfx_panel(a.x, a.y, a.w, a.h, S(22), C_PANEL, 242, 0xFFFFFF, 190);

    if (a_logo_sm.pixels)
        gfx_blit(&a_logo_sm, a.x + S(38), a.y + S(24));
    text_at(&g_font_title19, a.x + S(78), a.y + S(48), "ReverticalOS",
            C_INK, 250);
    auth_subtitle("Sign in to your account", cx, a.y + S(80));

    text_tracked(&g_font_ui13, a.ux, a.y + S(106), "USERNAME",
                 0x8A94AE, 220, 2);
    draw_field(a.ux, a.uy, a.uw, a.uh, auth_focus == 0,
               login_user, "your username", 0);

    text_tracked(&g_font_ui13, a.ux, a.y + S(166), "PASSWORD",
                 0x8A94AE, 220, 2);
    draw_field(a.px, a.py, a.pw, a.ph, auth_focus == 1,
               login_pass, "your password", 1);

    auth_error_text(cx, a.y + S(238));
    auth_button(a.bx, a.by, a.bw, a.bh, "Sign in");

    text_center(&g_font_ui13, cx, a.y + a.h - S(24),
                "Enter signs in  ·  Tab switches fields", 0x8A94AE, 200);
}

static void draw_lock_screen(void)
{
    gfx_copy_wallpaper();
    gfx_fill_rect_a(0, 0, screen_w, screen_h, 0x07101F, 145);

    AuthLayout a;
    auth_layout(&a, 1);
    int cx = a.x + a.w / 2;

    gfx_shadow_round(a.x, a.y + S(8), a.w, a.h, S(22), S(22),
                     0x030814, 110);
    gfx_panel(a.x, a.y, a.w, a.h, S(22), C_PANEL, 242, 0xFFFFFF, 190);

    const char *who = accounts_current();
    char initial = who[0] ? who[0] : 'R';
    if (initial >= 'a' && initial <= 'z')
        initial = (char)(initial - 'a' + 'A');
    gfx_fill_circle(cx, a.y + S(56), S(27), C_ACCENT, 80);
    gfx_fill_circle(cx, a.y + S(56), S(23), C_ACCENT, 255);
    char letter[2] = { initial, 0 };
    text_center(&g_font_title19, cx, a.y + S(64), letter, 0xFFFFFF, 255);

    text_center(&g_font_title19, cx, a.y + S(108), "Locked", C_INK, 250);
    text_center(&g_font_ui13, cx, a.y + S(128), who, 0x68748F, 230);

    text_tracked(&g_font_ui13, a.ux, a.y + S(152), "PASSWORD",
                 0x8A94AE, 220, 2);
    draw_field(a.px, a.py, a.pw, a.ph, 1, login_pass, "password", 1);

    auth_error_text(cx, a.y + S(222));
    auth_button(a.bx, a.by, a.bw, a.bh, "Unlock");

    text_center(&g_font_ui13, cx, a.y + a.h - S(22),
                "Press Enter to unlock", 0x8A94AE, 200);
}

static void power_card_rect(int *x, int *y, int *w, int *h)
{
    *w = S(470);
    *h = S(250);
    *x = (screen_w - *w) / 2;
    *y = (screen_h - *h) / 2;
}

static void draw_power_screen(void)
{
    gfx_copy_wallpaper();
    gfx_fill_rect_a(0, 0, screen_w, screen_h, 0x03101C, 175);

    int x, y, w, h;
    power_card_rect(&x, &y, &w, &h);
    int cx = x + w / 2;
    gfx_shadow_round(x, y + S(8), w, h, S(22), S(22), 0x02060F, 120);
    gfx_panel(x, y, w, h, S(22), C_PANEL, 242, 0xFFFFFF, 190);

    if (screen_mode == SCREEN_POWER) {
        /* Three orbiting dots make a tiny spinner without any timers. */
        for (int i = 0; i < 3; i++) {
            int a = ((int)(ui_now / 18) + i * 120) * 256 / 360;
            int px = cx + cos_i(a) * S(15) / 65536;
            int py = y + S(70) + sin_i(a) * S(15) / 65536;
            gfx_fill_circle(px, py, S(4), i == 0 ? C_ACCENT : 0x8FB2FF,
                            255 - i * 45);
        }
        text_center(&g_font_title19, cx, y + S(126),
                    power_action ? "Restarting" : "Shutting down",
                    C_INK, 250);
        text_center(&g_font_ui13, cx, y + S(150),
                    power_action ? "The machine will reset in a moment."
                                 : "Saving state and stopping the machine.",
                    0x68748F, 220);
        text_center(&g_font_ui13, cx, y + S(172),
                    power_action ? "Hold the reset button if nothing happens."
                                 : "ACPI is asked first; the reset line is the fallback.",
                    0x93A0B8, 190);
    } else {
        gfx_fill_circle(cx, y + S(64), S(24), 0xEAF6EF, 255);
        gfx_fill_circle(cx, y + S(64), S(17), C_GREEN, 255);
        line(cx - S(7), y + S(64), cx - S(1), y + S(70), 0xFFFFFF, 250);
        line(cx - S(1), y + S(70), cx + S(8), y + S(58), 0xFFFFFF, 250);

        text_center(&g_font_title19, cx, y + S(122),
                    "It is safe to switch off", C_INK, 250);
        text_center(&g_font_ui13, cx, y + S(146),
                    "The firmware did not accept the power-off request.",
                    0x68748F, 220);
        text_center(&g_font_ui13, cx, y + S(164),
                    "Close the VM or hold the power button to finish.",
                    0x93A0B8, 195);

        int bw = S(220), bh = S(38);
        int bx = cx - bw / 2, by = y + h - S(58);
        auth_button(bx, by, bw, bh, "Return to desktop");
    }
}

static void attempt_login(void)
{
    if (!login_user[0]) {
        path_copy(auth_error, "Enter your username", sizeof(auth_error));
        auth_focus = 0;
        return;
    }
    if (!login_pass[0]) {
        path_copy(auth_error, "Enter your password", sizeof(auth_error));
        auth_focus = 1;
        return;
    }
    if (!accounts_check(login_user, login_pass)) {
        path_copy(auth_error, "Incorrect username or password",
                  sizeof(auth_error));
        login_pass[0] = 0;
        auth_focus = 1;
        return;
    }

    auth_error[0] = 0;
    login_pass[0] = 0;
    screen_mode = SCREEN_DESKTOP;
    start_open = 0;
    notif_open = 0;
    char text[64];
    int n = 0;
    const char *p = "Signed in as ";
    while (*p && n < 63) text[n++] = *p++;
    for (int i = 0; accounts_current()[i] && n < 63; i++)
        text[n++] = accounts_current()[i];
    text[n] = 0;
    notif_push(text, C_GREEN);
}

static void attempt_unlock(void)
{
    if (!accounts_current_valid()) {
        screen_mode = SCREEN_LOGIN;
        auth_focus = 0;
        auth_error[0] = 0;
        return;
    }
    if (!login_pass[0]) {
        path_copy(auth_error, "Enter your password", sizeof(auth_error));
        return;
    }
    if (!accounts_check(accounts_current(), login_pass)) {
        path_copy(auth_error, "Incorrect password", sizeof(auth_error));
        login_pass[0] = 0;
        return;
    }
    auth_error[0] = 0;
    login_pass[0] = 0;
    screen_mode = SCREEN_DESKTOP;
    notif_push("Desktop unlocked", C_GREEN);
}

static void auth_append(char *buf, int max, int c, int lower)
{
    int n = (int)strlen(buf);
    if (n >= max - 1)
        return;
    if (c >= 'A' && c <= 'Z') {
        if (lower)
            c = c - 'A' + 'a';
    }
    buf[n] = (char)c;
    buf[n + 1] = 0;
}

static void auth_key(int c)
{
    if (screen_mode == SCREEN_LOGIN) {
        if (c == KEY_ESC) {
            auth_error[0] = 0;
            return;
        }
        if (c == KEY_TAB) {
            auth_focus = auth_focus ? 0 : 1;
            return;
        }
        if (c == KEY_ENTER) {
            if (auth_focus == 0) {
                if (!login_user[0]) {
                    path_copy(auth_error, "Enter your username",
                              sizeof(auth_error));
                    return;
                }
                auth_focus = 1;
                return;
            }
            attempt_login();
            return;
        }
        if (c == KEY_BACK) {
            char *buf = auth_focus ? login_pass : login_user;
            int n = (int)strlen(buf);
            if (n > 0) buf[n - 1] = 0;
            auth_error[0] = 0;
            return;
        }
        if (c >= 0x20 && c < 0x7F) {
            auth_error[0] = 0;
            if (auth_focus == 0) {
                if (c == ' ')
                    return;
                auth_append(login_user, sizeof(login_user), c, 1);
            } else {
                auth_append(login_pass, sizeof(login_pass), c, 0);
            }
        }
        return;
    }

    if (screen_mode == SCREEN_LOCK) {
        if (c == KEY_BACK || c == KEY_ESC) {
            login_pass[0] = 0;
            auth_error[0] = 0;
            return;
        }
        if (c == KEY_ENTER) {
            attempt_unlock();
            return;
        }
        if (c >= 0x20 && c < 0x7F) {
            auth_error[0] = 0;
            auth_append(login_pass, sizeof(login_pass), c, 0);
        }
        return;
    }

    if (screen_mode == SCREEN_POWER_SAFE) {
        if (c == KEY_ENTER || c == KEY_ESC || c == ' ')
            screen_mode = SCREEN_DESKTOP;
    }
}

static void screen_click(void)
{
    int mx = mouse_x, my = mouse_y;

    if (screen_mode == SCREEN_LOGIN) {
        AuthLayout a;
        auth_layout(&a, 0);
        if (point_in(a.ux, a.uy, a.uw, a.uh, mx, my)) {
            auth_focus = 0;
            return;
        }
        if (point_in(a.px, a.py, a.pw, a.ph, mx, my)) {
            auth_focus = 1;
            return;
        }
        if (point_in(a.bx, a.by, a.bw, a.bh, mx, my)) {
            attempt_login();
            return;
        }
        return;
    }

    if (screen_mode == SCREEN_LOCK) {
        AuthLayout a;
        auth_layout(&a, 1);
        if (point_in(a.px, a.py, a.pw, a.ph, mx, my))
            return;
        if (point_in(a.bx, a.by, a.bw, a.bh, mx, my)) {
            attempt_unlock();
            return;
        }
        return;
    }

    if (screen_mode == SCREEN_POWER_SAFE) {
        int x, y, w, h;
        power_card_rect(&x, &y, &w, &h);
        int bw = S(220), bh = S(38);
        int bx = x + (w - bw) / 2, by = y + h - S(58);
        if (point_in(bx, by, bw, bh, mx, my))
            screen_mode = SCREEN_DESKTOP;
    }
}

static void hover_refresh(void)
{
    nav_hover = -1;
    dock_hover = -1;
    close_hover = -1;
    min_hover = -1;
    start_hover = -1;
    notif_hover = -1;

    int mx = mouse_x, my = mouse_y;

    if (screen_mode != SCREEN_DESKTOP) {
        shortcut_hover = 0;
        return;
    }

    if (start_open)
        start_hover = start_row_hit(mx, my);

    if (notif_open) {
        int nx, ny, nw, nh;
        notif_panel_rect(&nx, &ny, &nw, &nh);
        if (point_in(nx, ny, nw, nh, mx, my) && notif_count > 0) {
            int row = S(34);
            for (int i = 0; i < notif_count; i++) {
                int ry = ny + S(44) + i * row;
                if (point_in(nx + S(8), ry, nw - S(16), row - S(3), mx, my))
                    notif_hover = i;
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (point_in(L.nav_x[i] - S(6), L.nav_y - S(6),
                     L.nav_size + S(12), L.nav_size + S(12), mx, my))
            nav_hover = i;
    }
    for (int i = 0; i < DOCK_APPS; i++) {
        if (point_in(L.dock_ix[i] - S(5), L.dock_iy - S(5),
                     L.dock_size + S(10), L.dock_size + S(10), mx, my))
            dock_hover = i;
    }

    shortcut_hover = 0;
    if (live_env) {
        int sx, sy, sw, sh;
        shortcut_rect(&sx, &sy, &sw, &sh);
        if (app_at(mx, my) < 0 && point_in(sx, sy, sw, sh, mx, my))
            shortcut_hover = 1;
    }

    int app = app_at(mx, my);
    if (app >= 0) {
        Window *w = &wins[app];
        if (point_in(w->x + w->w - S(40), w->y + S(14), S(20), S(20), mx, my))
            close_hover = app;
        if (point_in(w->x + w->w - S(70), w->y + S(14), S(20), S(20), mx, my))
            min_hover = app;
    }
}

static void handle_click(void)
{
    int mx = mouse_x, my = mouse_y;
    if (screen_mode != SCREEN_DESKTOP) {
        screen_click();
        return;
    }

    if (start_button_hit(mx, my)) {
        start_open = !start_open;
        notif_open = 0;
        start_sel = 0;
        return;
    }

    if (start_open) {
        int idx = start_row_hit(mx, my);
        if (idx >= 0) {
            start_activate(idx);
            return;
        }
        int sx, sy, sw, sh;
        start_rect(&sx, &sy, &sw, &sh);
        if (point_in(sx, sy, sw, sh, mx, my))
            return;
        start_open = 0;
    }

    if (notif_open) {
        int nx, ny, nw, nh;
        notif_panel_rect(&nx, &ny, &nw, &nh);
        if (point_in(nx, ny, nw, nh, mx, my)) {
            if (notif_count > 0) {
                int row = S(34);
                for (int i = 0; i < notif_count; i++) {
                    int ry = ny + S(44) + i * row;
                    if (point_in(nx + S(8), ry, nw - S(16),
                                 row - S(3), mx, my)) {
                        notifs[i].read = 1;
                        return;
                    }
                }
                int cy = ny + nh - S(30);
                const char *clear = "Clear all";
                int cw = tw(&g_font_ui13, clear) + S(18);
                int cx = nx + nw - cw - S(10);
                if (point_in(cx, cy - S(2), cw, S(22), mx, my)) {
                    notif_count = 0;
                    return;
                }
            }
            notif_hover = -1;
            return;
        }
        int bell_hit = point_in(L.nav_x[3] - S(6), L.nav_y - S(6),
                               L.nav_size + S(12), L.nav_size + S(12),
                               mx, my);
        notif_open = 0;
        if (bell_hit)
            return;
    }

    int under = app_at(mx, my);
    if (under != APP_BROWSER) {
        browser_focus_url = 0;
        browser_url_select = 0;
    }
    if (under != APP_SETTINGS)
        wifi_focus = 0;

    /* Windows, front to back. */
    for (int i = 0; i < APP_COUNT; i++) {
        int app = zorder[i];
        if (!window_visible(app)) continue;
        Window *w = &wins[app];
        if (!point_in(w->x, w->y, w->w, w->h, mx, my)) continue;

        if (point_in(w->x + w->w - S(40), w->y + S(14), S(20), S(20), mx, my)) {
            if (app == APP_INSTALLER && installer_installing()) {
                focus_app(app);
                return;
            }
            w->open = 0;
            w->minimized = 0;
            drag_app = -1;
            return;
        }
        if (point_in(w->x + w->w - S(70), w->y + S(14), S(20), S(20), mx, my)) {
            if (app == APP_INSTALLER && installer_installing()) {
                focus_app(app);
                return;
            }
            if (app == APP_CONSOLE)
                w->open = 0;           /* no dock slot to restore it */
            else
                w->minimized = 1;
            drag_app = -1;
            return;
        }
        if (app == APP_INSTALLER) {
            int bx = w->x + S(14), by = w->y + S(58);
            int bw = w->w - S(28), bh = w->h - S(72);
            if (app_installer_click(bx, by, bw, bh, mx, my)) {
                focus_app(app);
                return;
            }
        }
        if (app == APP_FILES) {
            int bx = w->x + S(14), by = w->y + S(58);
            int bw = w->w - S(28), bh = w->h - S(72);
            if (app_files_click(bx, by, bw, bh, mx, my)) {
                focus_app(app);
                return;
            }
        }
        if (app == APP_BROWSER) {
            int bx = w->x + S(14), by = w->y + S(58);
            int bw = w->w - S(28), bh = w->h - S(72);
            if (app_browser_click(bx, by, bw, bh, mx, my)) {
                wifi_focus = 0;
                focus_app(app);
                return;
            }
        }
        if (app == APP_SETTINGS) {
            int bx = w->x + S(14), by = w->y + S(58);
            int bw = w->w - S(28), bh = w->h - S(72);
            browser_focus_url = 0;
            if (app_settings_click(bx, by, bw, bh, mx, my)) {
                focus_app(app);
                return;
            }
        }

        focus_app(app);
        if (my < w->y + S(50)) {
            drag_app = app;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        }
        return;
    }

    if (live_env) {
        int sx, sy, sw, sh;
        shortcut_rect(&sx, &sy, &sw, &sh);
        if (point_in(sx, sy, sw, sh, mx, my)) {
            inst_wipe_confirm = 0;
            toggle_app(APP_INSTALLER);
            return;
        }
    }

    for (int i = 0; i < 4; i++) {
        if (point_in(L.nav_x[i] - S(6), L.nav_y - S(6),
                     L.nav_size + S(12), L.nav_size + S(12), mx, my)) {
            start_open = 0;
            if (i == 0) {
                toggle_app(APP_AI);
            } else if (i == 1) {
                if (any_visible_window()) {
                    panels_hidden = 1;
                    for (int a = 0; a < APP_COUNT; a++)
                        if (wins[a].open) wins[a].minimized = 1;
                } else {
                    panels_hidden = 0;
                    for (int a = 0; a < APP_COUNT; a++)
                        if (wins[a].open) wins[a].minimized = 0;
                }
                drag_app = -1;
            } else if (i == 2) {
                toggle_app(APP_FILES);
            } else {
                notif_open = !notif_open;
            }
            return;
        }
    }

    for (int i = 0; i < DOCK_APPS; i++) {
        if (point_in(L.dock_ix[i] - S(5), L.dock_iy - S(5),
                     L.dock_size + S(10), L.dock_size + S(10), mx, my)) {
            toggle_app(i);
            return;
        }
    }

    if (point_in(L.card_x, L.card_y, L.card_w, L.card_h, mx, my)) {
        toggle_app(APP_SETTINGS);
        return;
    }

    (void)panels_hidden;
}

static int ui_keyboard(void)
{
    int c;
    int changed = 0;
    while ((c = kbd_getc()) != 0) {
        changed = 1;
        if (screen_mode != SCREEN_DESKTOP) {
            auth_key(c);
            continue;
        }
        if (start_open) {
            if (c == KEY_ESC) {
                start_open = 0;
            } else if (c == KEY_UP) {
                start_sel = (start_sel + START_ENTRIES - 1) % START_ENTRIES;
            } else if (c == KEY_DOWN) {
                start_sel = (start_sel + 1) % START_ENTRIES;
            } else if (c == KEY_ENTER || c == ' ') {
                start_activate(start_sel);
            }
            continue;
        }
        if (window_visible(APP_INSTALLER) && zorder[0] == APP_INSTALLER &&
            inst_stage == 0) {
            inst_account_key(c);
            continue;
        }
        if (window_visible(APP_BROWSER) && zorder[0] == APP_BROWSER &&
            browser_focus_url) {
            if (c == '\n') {
                browser_focus_url = 0;
                browser_url_select = 0;
                browser_navigate(browser_url,
                                 wins[APP_BROWSER].w - S(28), 1);
            } else if (c == '\b') {
                browser_url_trim();
            } else if (c == KEY_ESC) {
                browser_focus_url = 0;
                browser_url_select = 0;
            } else if (c == 12) {
                /* Ctrl+L keeps the field focused */
            } else if (c >= 0x20 && c < 0x7F) {
                browser_url_append(c);
            }
            continue;
        }

        if (window_visible(APP_SETTINGS) && zorder[0] == APP_SETTINGS &&
            wifi_focus) {
            int n = (int)strlen(wifi_pass);
            if (c == '\n') {
                settings_connect();
            } else if (c == '\b') {
                if (n > 0) wifi_pass[n - 1] = 0;
            } else if (c == KEY_ESC) {
                wifi_focus = 0;
            } else if (c >= 0x20 && c < 0x7F && n < (int)sizeof(wifi_pass) - 1) {
                wifi_pass[n] = (char)c;
                wifi_pass[n + 1] = 0;
            }
            continue;
        }

        if (c == 12 && window_visible(APP_BROWSER) &&
            zorder[0] == APP_BROWSER) {
            browser_focus_url = 1;
            browser_url_select = 1;
            continue;
        }

        if (!window_visible(APP_BROWSER) || zorder[0] != APP_BROWSER)
            continue;

        switch (c) {
        case KEY_UP:    browser_scroll_step(-1); break;
        case KEY_DOWN:  browser_scroll_step(1); break;
        case KEY_PGUP:  browser_scroll_step(-(browser_visible_lines - 2)); break;
        case KEY_PGDN:  browser_scroll_step(browser_visible_lines - 2); break;
        case KEY_HOME:  browser_scroll = 0; break;
        case KEY_END:   browser_scroll = browser_line_count; break;
        case KEY_ESC:
            browser_focus_url = 0;
            break;
        default:
            if (c >= '1' && c <= '8')
                browser_goto_link(c - '1');
            break;
        }
    }
    return changed;
}

static int ui_update(void)
{
    ui_now = timer_ms();
    int dirty = ui_keyboard();

    if (installer_installing()) {
        installer_poll();
        dirty = 1;
    }
    if (installer.phase != inst_last_phase) {
        inst_last_phase = installer.phase;
        if (installer.phase == INSTALL_DONE) {
            live_env = !installed_marker();
            files_refresh();
            if (installer.boot_dev < 0)
                notif_push("Installation complete - boot the target disk",
                           C_GREEN);
            else if (installer.target == installer.boot_dev)
                notif_push("Installation complete - restart to sign in",
                           C_GREEN);
            else
                notif_push("Installation complete on the target disk",
                           C_GREEN);
        } else if (installer.phase == INSTALL_FAILED) {
            notif_push("Installation failed - check the installer log",
                       C_RED);
        }
        dirty = 1;
    }

    if (screen_mode == SCREEN_POWER) {
        dirty = 1;                  /* spinner */
        if (power_at && ui_now - power_at >= 750) {
            if (power_action)
                power_reboot();     /* never returns */
            power_shutdown();       /* returns only on failure */
            power_at = 0;
            screen_mode = SCREEN_POWER_SAFE;
        }
    }

    if (ui_now - last_rtc_ms >= 1000) {
        last_rtc_ms = ui_now;
        char before[8];
        memcpy(before, clock_text, sizeof(before));
        clock_update();
        if (memcmp(before, clock_text, sizeof(before)) != 0)
            dirty = 1;

        NetIf *eth = net_wired();
        if (eth && eth->up && eth->ip && !seen_eth) {
            seen_eth = 1;
            char text[72];
            int n = 0;
            const char *p = "Ethernet connected - ";
            while (*p && n < 71) text[n++] = *p++;
            const char *ip = net_ip_text(eth->ip);
            while (*ip && n < 71) text[n++] = *ip++;
            text[n] = 0;
            notif_push(text, C_TEAL);
            dirty = 1;
        }
        if (wifi_connected() && !seen_wifi) {
            seen_wifi = 1;
            char text[72];
            int n = 0;
            const char *p = "Wi-Fi connected - ";
            while (*p && n < 71) text[n++] = *p++;
            const char *ssid = wifi_ssid();
            while (*ssid && n < 71) text[n++] = *ssid++;
            text[n] = 0;
            notif_push(text, C_PURPLE);
            dirty = 1;
        }
    }

    int moved = mouse_x != last_mx || mouse_y != last_my;
    if (moved) {
        last_mx = mouse_x;
        last_my = mouse_y;
        dirty = 1;
    }

    int old_nav = nav_hover, old_dock = dock_hover;
    int old_close = close_hover, old_min = min_hover;
    int old_start = start_hover, old_notif = notif_hover;
    hover_refresh();
    if (old_nav != nav_hover || old_dock != dock_hover ||
        old_close != close_hover || old_min != min_hover ||
        old_start != start_hover || old_notif != notif_hover)
        dirty = 1;

    if (drag_app >= 0 && screen_mode == SCREEN_DESKTOP) {
        if (mouse_buttons & 1) {
            Window *w = &wins[drag_app];
            w->x = iclamp(mouse_x - drag_dx, S(24) - w->w, screen_w - S(24));
            w->y = iclamp(mouse_y - drag_dy, S(10), screen_h - S(90));
            dirty = 1;
        } else {
            drag_app = -1;
        }
    }

    if (mouse_clicked_left) {
        mouse_clicked_left = 0;
        handle_click();
        dirty = 1;
    }

    for (int i = 0; i < APP_COUNT; i++) {
        if (wins[i].open && ui_now - wins[i].open_time < 200)
            dirty = 1;
    }

    if (window_visible(APP_BROWSER) && ui_now - last_anim_ms >= 250) {
        last_anim_ms = ui_now;
        dirty = 1;
    }

    if ((screen_mode == SCREEN_LOGIN || screen_mode == SCREEN_LOCK) &&
        ui_now - last_anim_ms >= 420) {
        last_anim_ms = ui_now;
        dirty = 1;
    }

    int toasting = toast_active();
    static int toast_was_active;
    if (toasting || toast_was_active) {
        if (ui_now - last_anim_ms >= 250) {
            last_anim_ms = ui_now;
            dirty = 1;
        }
    }
    toast_was_active = toasting;

    return dirty;
}

/* ------------------------------------------------------------------------- */
/*  Frame                                                                     */
/* ------------------------------------------------------------------------- */
void ui_render(void)
{
    if (screen_mode == SCREEN_LOGIN) {
        draw_login_screen();
        draw_cursor();
        return;
    }
    if (screen_mode == SCREEN_LOCK) {
        draw_lock_screen();
        draw_cursor();
        return;
    }
    if (screen_mode == SCREEN_POWER || screen_mode == SCREEN_POWER_SAFE) {
        draw_power_screen();
        draw_cursor();
        return;
    }

    gfx_copy_wallpaper();
    draw_shortcut();

    for (int i = APP_COUNT - 1; i >= 0; i--)
        draw_window(zorder[i]);

    draw_notifications();
    draw_start_menu();
    draw_pill();
    draw_dock();
    draw_card();
    draw_toasts();
    draw_cursor();
}

void ui_run(void)
{
    u64 last_draw = 0;
    for (;;) {
        usb_poll();
        mouse_poll();
        ui_now = timer_ms();
        int dirty = ui_update();
        if (dirty && (ui_now - last_draw >= 16 || ui_now < last_draw)) {
            ui_render();
            gfx_present();
            last_draw = ui_now;
        } else {
            __asm__ volatile ("pause");
        }
    }
}
