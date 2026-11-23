/* System Info.rvx - inspects the running kernel through its module API. */

typedef unsigned int u32;

extern void rv_print(const char *s);
extern u32 rv_tick_ms(void);
extern void *rv_alloc(u32 bytes);
extern u32 rv_rand(void);
extern u32 rv_net_state(void);
extern const char *rv_net_ip(void);
extern int rv_wifi_connected(void);

static void put_u32(const char *label, u32 value)
{
    char buf[16];
    int n = 0;
    if (value == 0)
        buf[n++] = '0';
    while (value) {
        buf[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    rv_print(label);
    char c[2];
    c[1] = 0;
    while (n) {
        c[0] = buf[--n];
        rv_print(c);
    }
    rv_print("\n");
}

static void put_size(u32 bytes)
{
    put_u32("  heap block: ", bytes);
}

int _start(void)
{
    rv_print("ReverticalOS system probe\n");
    rv_print("-------------------------\n");

    put_u32("uptime ms: ", rv_tick_ms());
    put_u32("net state: ", rv_net_state());

    rv_print("ip address: ");
    rv_print(rv_net_ip());
    rv_print("\n");

    put_u32("wifi joined: ", (u32)rv_wifi_connected());
    put_u32("random: ", rv_rand());

    void *a = rv_alloc(16);
    void *b = rv_alloc(1024);
    put_u32("first allocation: ", (u32)(unsigned long)a);
    put_u32("second allocation: ", (u32)(unsigned long)b);
    put_size(1024);
    (void)a;
    (void)b;

    rv_print("module formats: RVEX executable, RVLB library\n");
    rv_print("probe complete\n");
    return 0;
}
