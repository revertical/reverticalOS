#ifndef RVOS_SHA1_H
#define RVOS_SHA1_H

#include "types.h"

typedef struct {
    u32 state[5];
    u64 bits;
    u8 block[64];
    u32 used;
} Sha1;

void sha1_init(Sha1 *c);
void sha1_update(Sha1 *c, const void *data, u32 len);
void sha1_final(Sha1 *c, u8 out[20]);

void hmac_sha1(const u8 *key, u32 key_len, const void *data, u32 len,
               u8 out[20]);
/* WPA2 passphrase -> 256-bit PMK (PBKDF2-HMAC-SHA1, 4096 rounds). */
void pbkdf2_sha1(const char *pass, const char *ssid, u8 out[32]);

#endif
