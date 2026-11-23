#ifndef RVOS_ACCOUNTS_H
#define RVOS_ACCOUNTS_H

#include "types.h"

#define USER_NAME_MAX   20
#define USER_PASS_MIN   4
#define USER_PASS_MAX   40
#define ACCOUNTS_PATH   "/System/users"
#define ACCOUNTS_MAGIC  "RVUS1"

/* Validate an account name or password. Returns 0 on success, or -1 with a
   short human readable message in err (when err is not NULL). */
int accounts_validate_name(const char *name, char *err, int max);
int accounts_validate_pass(const char *pass, char *err, int max);

/* PBKDF2-HMAC-SHA1(password, "RVOS1:" + lowercase(user)), 32 bytes as hex. */
void accounts_hash(const char *user, const char *pass, char out_hex[65]);

/* Read /System/users into memory. Safe to call when the file is missing. */
int  accounts_init(void);
int  accounts_exists(void);
int  accounts_count(void);
int  accounts_get(int index, char *name, int max);

/* Compare a password against the stored hash. On success the account also
   becomes the current user. Returns 1 for a match, 0 otherwise. */
int  accounts_check(const char *user, const char *pass);

/* Append an account and rewrite /System/users. Returns 0 on success. */
int  accounts_add(const char *user, const char *pass);

const char *accounts_current(void);
int  accounts_current_valid(void);

#endif
