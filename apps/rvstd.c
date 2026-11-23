/* rvstd.lib - a small runtime library for ReverticalOS modules.
 *
 * Compiled as a freestanding object and packed into RVLB by tools/rvlink.py.
 * Every symbol exported here is resolved by name when another module imports
 * it, so the library must not depend on the kernel itself.
 */

typedef unsigned int u32;

const char *rvstd_banner(void)
{
    return "rvstd.lib 1.0: RVLB loaded, exports resolved\n";
}

u32 rvstd_strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

void rvstd_itoa(u32 value, char *out)
{
    char tmp[12];
    int n = 0;
    if (value == 0)
        tmp[n++] = '0';
    while (value) {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    int k = 0;
    while (n)
        out[k++] = tmp[--n];
    out[k] = 0;
}

u32 rvstd_fib(u32 n)
{
    u32 a = 0, b = 1;
    while (n--) {
        u32 t = a + b;
        a = b;
        b = t;
    }
    return a;
}

u32 rvstd_gcd(u32 a, u32 b)
{
    while (b) {
        u32 t = a % b;
        a = b;
        b = t;
    }
    return a;
}
