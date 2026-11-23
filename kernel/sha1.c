/* SHA-1, HMAC-SHA-1 and the WPA2 PBKDF2 derivation.
 *
 * WPA2-PSK does not use the passphrase directly: the PMK is
 * PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 rounds, 256 bits).  The same
 * routines feed the 4-way handshake MIC.
 */

#include "sha1.h"
#include "string.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void transform(Sha1 *c, const u8 block[64])
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = c->state[0], b = c->state[1], d = c->state[2];
    u32 e = c->state[3], f = c->state[4];

    for (int i = 0; i < 80; i++) {
        u32 k, t;
        if (i < 20) {
            t = (b & d) | (~b & e);
            k = 0x5A827999u;
        } else if (i < 40) {
            t = b ^ d ^ e;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            t = (b & d) | (b & e) | (d & e);
            k = 0x8F1BBCDCu;
        } else {
            t = b ^ d ^ e;
            k = 0xCA62C1D6u;
        }
        u32 tmp = ROL(a, 5) + t + f + k + w[i];
        f = e;
        e = d;
        d = ROL(b, 30);
        b = a;
        a = tmp;
    }

    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += d;
    c->state[3] += e;
    c->state[4] += f;
}

void sha1_init(Sha1 *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xEFCDAB89u;
    c->state[2] = 0x98BADCFEu;
    c->state[3] = 0x10325476u;
    c->state[4] = 0xC3D2E1F0u;
    c->bits = 0;
    c->used = 0;
}

void sha1_update(Sha1 *c, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    c->bits += (u64)len * 8;

    while (len) {
        u32 space = 64 - c->used;
        u32 take = len < space ? len : space;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;
        if (c->used == 64) {
            transform(c, c->block);
            c->used = 0;
        }
    }
}

void sha1_final(Sha1 *c, u8 out[20])
{
    u8 pad[72];
    u32 padlen;
    u64 bits = c->bits;

    pad[0] = 0x80;
    padlen = (c->used < 56) ? (56 - c->used) : (120 - c->used);
    memset(pad + 1, 0, padlen - 1);
    for (int i = 0; i < 8; i++)
        pad[padlen + i] = (u8)(bits >> (56 - i * 8));
    sha1_update(c, pad, padlen + 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(c->state[i] >> 24);
        out[i * 4 + 1] = (u8)(c->state[i] >> 16);
        out[i * 4 + 2] = (u8)(c->state[i] >> 8);
        out[i * 4 + 3] = (u8)c->state[i];
    }
}

void hmac_sha1(const u8 *key, u32 key_len, const void *data, u32 len,
               u8 out[20])
{
    u8 k[64];
    u8 ipad[64], opad[64];
    u8 inner[20];
    Sha1 c;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, data, len);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

static void pbkdf2_block(const u8 *pass, u32 plen, const u8 *salt, u32 slen,
                         u32 index, u8 out[20])
{
    u8 msg[64];
    u8 u[20], t[20];

    memcpy(msg, salt, slen);
    msg[slen] = (u8)(index >> 24);
    msg[slen + 1] = (u8)(index >> 16);
    msg[slen + 2] = (u8)(index >> 8);
    msg[slen + 3] = (u8)index;

    hmac_sha1(pass, plen, msg, slen + 4, u);
    memcpy(t, u, 20);
    for (u32 round = 1; round < 4096; round++) {
        hmac_sha1(pass, plen, u, 20, u);
        for (int i = 0; i < 20; i++) t[i] ^= u[i];
    }
    memcpy(out, t, 20);
}

void pbkdf2_sha1(const char *pass, const char *ssid, u8 out[32])
{
    u8 salt[40];
    u32 plen = strlen(pass);
    u32 slen = strlen(ssid);

    if (plen > 63) plen = 63;
    if (slen > 32) slen = 32;
    memcpy(salt, ssid, slen);

    u8 tail[20];
    pbkdf2_block((const u8 *)pass, plen, salt, slen, 1, out);
    pbkdf2_block((const u8 *)pass, plen, salt, slen, 2, tail);
    memcpy(out + 20, tail, 12);
}
