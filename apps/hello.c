/* Hello.rvx - demonstrates a real RVEX executable importing an RVLB library.
 *
 * The kernel loader maps this image, rebases its absolute references,
 * resolves rv_print/rv_tick_ms from the kernel API and rvstd_* from
 * /System/Libs/rvstd.lib, then calls _start() on a private stack.
 */

typedef unsigned int u32;

extern void rv_print(const char *s);
extern u32 rv_tick_ms(void);
extern void *rv_alloc(u32 bytes);
extern u32 rv_rand(void);

extern const char *rvstd_banner(void);
extern u32 rvstd_strlen(const char *s);
extern void rvstd_itoa(u32 value, char *out);
extern u32 rvstd_fib(u32 n);
extern u32 rvstd_gcd(u32 a, u32 b);

static void print_u32(const char *label, u32 value)
{
    char buf[16];
    rv_print(label);
    rvstd_itoa(value, buf);
    rv_print(buf);
    rv_print("\n");
}

int _start(void)
{
    rv_print("Hello from an RVEX executable.\n");
    rv_print(rvstd_banner());

    print_u32("fib(20) = ", rvstd_fib(20));
    print_u32("gcd(1071, 462) = ", rvstd_gcd(1071, 462));
    print_u32("kernel tick: ", rv_tick_ms());
    print_u32("random: ", rv_rand() % 100000);

    char *heap = (char *)rv_alloc(64);
    if (heap) {
        for (int i = 0; i < 26; i++)
            heap[i] = (char)('A' + i);
        heap[26] = 0;
        rv_print("rv_alloc(64) -> ");
        rv_print(heap);
        rv_print("\n");
    }

    print_u32("banner length: ", rvstd_strlen(rvstd_banner()));

    return 0;
}
