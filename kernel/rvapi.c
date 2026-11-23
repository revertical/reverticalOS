/* Kernel API exported to RVEX/RVLB modules.
 *
 * Names here are the import names a module links against.  Keeping the table
 * explicit means the loader can resolve modules without a kernel symbol table.
 */

#include "rvmod.h"
#include "net.h"
#include "wifi.h"
#include "timer.h"
#include "mouse.h"
#include "string.h"
#include "serial.h"
#include "panic.h"

#define API_OUTPUT_MAX 8192
#define API_HEAP_MAX   (128u * 1024u)

static char api_out[API_OUTPUT_MAX];
static u32 api_out_len;
static u8 api_heap[API_HEAP_MAX];
static u32 api_heap_used;
static u32 api_rand_state = 0x52564F53u;

void rvapi_reset(void)
{
    api_out_len = 0;
    api_out[0] = 0;
    api_heap_used = 0;
}

void rv_print(const char *s)
{
    if (!s)
        return;
    while (*s) {
        if (api_out_len < API_OUTPUT_MAX - 1)
            api_out[api_out_len++] = *s;
        serial_putc(*s);
        s++;
    }
    api_out[api_out_len] = 0;
}

u32 rv_tick_ms(void)
{
    return (u32)timer_ms();
}

void *rv_alloc(u32 bytes)
{
    u32 aligned = (bytes + 15u) & ~15u;
    if (aligned == 0 || api_heap_used + aligned > API_HEAP_MAX)
        return 0;
    void *p = api_heap + api_heap_used;
    api_heap_used += aligned;
    return p;
}

u32 rv_rand(void)
{
    api_rand_state = api_rand_state * 1664525u + 1013904223u;
    return api_rand_state >> 8;
}

u32 rv_net_state(void)
{
    if (net_wired() && net_wired()->up && net_wired()->ip)
        return 1;
    if (wifi_connected())
        return 2;
    return 0;
}

const char *rv_net_ip(void)
{
    if (net_wired() && net_wired()->up && net_wired()->ip)
        return net_ip_text(net_wired()->ip);
    if (net_wifi() && net_wifi()->up && net_wifi()->ip)
        return net_ip_text(net_wifi()->ip);
    return "0.0.0.0";
}

int rv_wifi_connected(void)
{
    return wifi_connected();
}

const char *rv_ps2_status(void)
{
    return mouse_status();
}

static void rv_panic(const char *message)
{
    panic(message && message[0] ? message : "module requested a panic");
}

static const RvApiSymbol api[] = {
    { "rv_print",         (uintptr_t)rv_print },
    { "rv_tick_ms",       (uintptr_t)rv_tick_ms },
    { "rv_alloc",         (uintptr_t)rv_alloc },
    { "rv_rand",          (uintptr_t)rv_rand },
    { "rv_net_state",     (uintptr_t)rv_net_state },
    { "rv_net_ip",        (uintptr_t)rv_net_ip },
    { "rv_wifi_connected",(uintptr_t)rv_wifi_connected },
    { "rv_ps2_status",    (uintptr_t)rv_ps2_status },
    { "rv_panic",         (uintptr_t)rv_panic },
};

int rvapi_init(void)
{
    api_out_len = 0;
    api_out[0] = 0;
    api_heap_used = 0;
    return (int)(sizeof(api) / sizeof(api[0]));
}

int rvapi_count(void)
{
    return (int)(sizeof(api) / sizeof(api[0]));
}

const RvApiSymbol *rvapi_symbol(int index)
{
    if (index < 0 || index >= rvapi_count())
        return 0;
    return &api[index];
}

const char *rvmod_output(void) { return api_out; }
int rvmod_output_len(void) { return (int)api_out_len; }
