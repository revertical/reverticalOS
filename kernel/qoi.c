#include "qoi.h"

static inline u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int qoi_decode(const u8 *data, u32 length, u32 *out, u32 max_pixels,
               u32 *out_width, u32 *out_height)
{
    if (length < 14 || data[0] != 'q' || data[1] != 'o' ||
        data[2] != 'i' || data[3] != 'f')
        return 0;

    u32 w = be32(data + 4);
    u32 h = be32(data + 8);
    u8 channels = data[12];
    if (w == 0 || h == 0 || (channels != 3 && channels != 4))
        return 0;
    if (w * h > max_pixels)
        return 0;

    u32 index[64];
    for (int i = 0; i < 64; i++) index[i] = 0u;
    u32 px = 0xFF000000u;
    u8 r = 0, g = 0, b = 0, a = 255;
    u32 pos = 14;
    u32 total = w * h;
    u32 run = 0;
    u32 p = 0;

    while (p < total) {
        if (run > 0) {
            run--;
        } else {
            if (pos >= length) return 0;
            u8 op = data[pos++];
            if (op == 0xFE) {
                if (pos + 3 > length) return 0;
                r = data[pos];
                g = data[pos + 1];
                b = data[pos + 2];
                pos += 3;
            } else if (op == 0xFF) {
                if (pos + 4 > length) return 0;
                r = data[pos];
                g = data[pos + 1];
                b = data[pos + 2];
                a = data[pos + 3];
                pos += 4;
            } else if ((op & 0xC0) == 0x00) {
                px = index[op & 0x3F];
                r = (u8)(px >> 16);
                g = (u8)(px >> 8);
                b = (u8)px;
                a = (u8)(px >> 24);
            } else if ((op & 0xC0) == 0x40) {
                r = (u8)(r + ((op >> 4) & 3) - 2);
                g = (u8)(g + ((op >> 2) & 3) - 2);
                b = (u8)(b + (op & 3) - 2);
            } else if ((op & 0xC0) == 0x80) {
                if (pos >= length) return 0;
                u8 op2 = data[pos++];
                i8 dg = (i8)((op & 0x3F) - 32);
                r = (u8)(r + dg + ((op2 >> 4) & 0xF) - 8);
                g = (u8)(g + dg);
                b = (u8)(b + dg + (op2 & 0xF) - 8);
            } else {
                run = op & 0x3F;
            }
            px = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b;
            index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = px;
        }
        out[p++] = px;
    }

    *out_width = w;
    *out_height = h;
    return 1;
}
