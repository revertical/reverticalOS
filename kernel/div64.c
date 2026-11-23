/* 64-bit division helpers.
 *
 * The kernel is linked freestanding and there is no 32-bit libgcc on the
 * build host, so provide the two routines gcc emits for u64 arithmetic.
 */
#include "types.h"

static u64 udivmod64(u64 num, u64 den, u64 *rem)
{
    u64 quot = 0;
    u64 rest = 0;
    if (!den) {
        if (rem) *rem = 0;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        rest = (rest << 1) | ((num >> i) & 1u);
        if (rest >= den) {
            rest -= den;
            quot |= ((u64)1 << i);
        }
    }
    if (rem) *rem = rest;
    return quot;
}

u64 __udivdi3(u64 num, u64 den)
{
    return udivmod64(num, den, 0);
}

u64 __umoddi3(u64 num, u64 den)
{
    u64 rem = 0;
    udivmod64(num, den, &rem);
    return rem;
}

u64 __udivmoddi4(u64 num, u64 den, u64 *rem)
{
    return udivmod64(num, den, rem);
}
