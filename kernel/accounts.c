/* ReverticalOS user accounts.
 *
 * Accounts live in /System/users, a tiny text database:
 *
 *     RVUS1
 *     alice 2b1f... (64 hex characters)
 *     bob   91ce...
 *
 * The digest is PBKDF2-HMAC-SHA1 with the salt "RVOS1:" followed by the
 * lowercase user name. There is no shadow file, no /etc/passwd and no libc;
 * the same SHA-1 code the Wi-Fi stack uses derives the password key.
 */
#include "accounts.h"
#include "rvfs.h"
#include "sha1.h"
#include "string.h"
#include "serial.h"

#define USERS_MAX 16
#define USERS_BUF 2048

typedef struct {
    char name[USER_NAME_MAX + 1];
    char hash[65];
} UserRec;

static UserRec users[USERS_MAX];
static int user_count;
static char current_user[USER_NAME_MAX + 1];
static char file_buf[USERS_BUF];

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static void copy_limited(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void set_error(char *err, int max, const char *text)
{
    if (!err || max <= 0)
        return;
    copy_limited(err, text, max);
}

int accounts_validate_name(const char *name, char *err, int max)
{
    int n = (int)strlen(name);
    if (n < 2) {
        set_error(err, max, "Use at least 2 characters");
        return -1;
    }
    if (n > USER_NAME_MAX) {
        set_error(err, max, "Use at most 20 characters");
        return -1;
    }
    if (!(name[0] >= 'a' && name[0] <= 'z')) {
        set_error(err, max, "Start with a lowercase letter");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            continue;
        set_error(err, max, "Use a-z, 0-9, dot, dash or underscore");
        return -1;
    }
    return 0;
}

int accounts_validate_pass(const char *pass, char *err, int max)
{
    int n = (int)strlen(pass);
    if (n < USER_PASS_MIN) {
        set_error(err, max, "Passwords need at least 4 characters");
        return -1;
    }
    if (n > USER_PASS_MAX) {
        set_error(err, max, "Passwords can be at most 40 characters");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pass[i];
        if (c < 0x20 || c > 0x7E) {
            set_error(err, max, "Use printable characters only");
            return -1;
        }
    }
    return 0;
}

void accounts_hash(const char *user, const char *pass, char out_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    char salt[40];
    int n = 0;
    const char *prefix = "RVOS1:";
    while (*prefix && n < 38)
        salt[n++] = *prefix++;
    for (int i = 0; user[i] && n < 39; i++)
        salt[n++] = lower_ascii(user[i]);
    salt[n] = 0;

    u8 digest[32];
    pbkdf2_sha1(pass, salt, digest);
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 15];
    }
    out_hex[64] = 0;
    memset(digest, 0, sizeof(digest));
}

int accounts_exists(void)
{
    RvfsStat st;
    return rvfs_mounted() && rvfs_stat(ACCOUNTS_PATH, &st) == RVFS_OK &&
           st.type == 1;
}

/* Parse file_buf (already NUL terminated) into users[]. */
static void parse_users(void)
{
    user_count = 0;
    int len = (int)strlen(file_buf);
    if (len < 6 || memcmp(file_buf, ACCOUNTS_MAGIC, 5) != 0)
        return;

    int pos = 0;
    while (pos < len && file_buf[pos] != '\n')
        pos++;
    if (pos < len)
        pos++;

    while (pos < len && user_count < USERS_MAX) {
        int start = pos;
        while (pos < len && file_buf[pos] != '\n')
            pos++;
        int end = pos;
        if (pos < len)
            pos++;
        while (end > start && (file_buf[end - 1] == '\r' ||
                               file_buf[end - 1] == ' '))
            end--;

        int sp = -1;
        for (int i = start; i < end; i++) {
            if (file_buf[i] == ' ') { sp = i; break; }
        }
        if (sp <= start || sp - start > USER_NAME_MAX)
            continue;
        if (end - sp - 1 != 64)
            continue;

        UserRec *u = &users[user_count];
        int n = sp - start;
        for (int i = 0; i < n; i++)
            u->name[i] = file_buf[start + i];
        u->name[n] = 0;
        for (int i = 0; i < 64; i++)
            u->hash[i] = lower_ascii(file_buf[sp + 1 + i]);
        u->hash[64] = 0;
        user_count++;
    }
}

static int refresh(void)
{
    user_count = 0;
    if (!rvfs_mounted())
        return -1;

    RvfsStat st;
    if (rvfs_stat(ACCOUNTS_PATH, &st) != RVFS_OK || st.type != 1)
        return -1;
    if (st.size >= USERS_BUF)
        return -1;

    int n = rvfs_read(ACCOUNTS_PATH, 0, file_buf, USERS_BUF - 1);
    if (n < 0)
        return -1;
    file_buf[n] = 0;
    parse_users();
    return 0;
}

int accounts_init(void)
{
    current_user[0] = 0;
    if (refresh() != 0)
        return -1;
    if (user_count > 0)
        copy_limited(current_user, users[0].name, sizeof(current_user));
    serial_write("accounts: ");
    serial_dec((u32)user_count);
    serial_write(" user(s)\r\n");
    return user_count;
}

int accounts_count(void)
{
    return user_count;
}

int accounts_get(int index, char *name, int max)
{
    if (index < 0 || index >= user_count || !name)
        return -1;
    copy_limited(name, users[index].name, max);
    return 0;
}

const char *accounts_current(void)
{
    return current_user;
}

int accounts_current_valid(void)
{
    if (!current_user[0])
        return 0;
    for (int i = 0; i < user_count; i++)
        if (strcmp(users[i].name, current_user) == 0)
            return 1;
    return 0;
}

int accounts_check(const char *user, const char *pass)
{
    char clean[USER_NAME_MAX + 1];
    int n = 0;
    while (user[n] && n < USER_NAME_MAX) {
        clean[n] = lower_ascii(user[n]);
        n++;
    }
    clean[n] = 0;
    if (!clean[0] || !pass)
        return 0;

    if (refresh() != 0)
        return 0;

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, clean) != 0)
            continue;
        char hex[65];
        accounts_hash(clean, pass, hex);
        int diff = 0;
        for (int k = 0; k < 64; k++)
            diff |= hex[k] != users[i].hash[k];
        memset(hex, 0, sizeof(hex));
        if (diff == 0) {
            copy_limited(current_user, clean, sizeof(current_user));
            return 1;
        }
        return 0;
    }
    return 0;
}

int accounts_add(const char *user, const char *pass)
{
    if (accounts_validate_name(user, 0, 0) != 0 ||
        accounts_validate_pass(pass, 0, 0) != 0)
        return -1;
    if (!rvfs_mounted())
        return -1;

    refresh();                      /* keep any existing accounts */

    char clean[USER_NAME_MAX + 1];
    int n = 0;
    while (user[n] && n < USER_NAME_MAX) {
        clean[n] = lower_ascii(user[n]);
        n++;
    }
    clean[n] = 0;

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, clean) == 0)
            return -1;              /* name is already taken */
    }
    if (user_count >= USERS_MAX)
        return -1;

    char hex[65];
    accounts_hash(clean, pass, hex);
    copy_limited(users[user_count].name, clean,
                 sizeof(users[user_count].name));
    copy_limited(users[user_count].hash, hex, sizeof(users[user_count].hash));
    user_count++;

    int p = 0;
    const char *magic = ACCOUNTS_MAGIC "\n";
    while (*magic && p < USERS_BUF - 1)
        file_buf[p++] = *magic++;
    for (int i = 0; i < user_count; i++) {
        const char *s = users[i].name;
        while (*s && p < USERS_BUF - 1)
            file_buf[p++] = *s++;
        if (p < USERS_BUF - 1)
            file_buf[p++] = ' ';
        s = users[i].hash;
        while (*s && p < USERS_BUF - 1)
            file_buf[p++] = *s++;
        if (p < USERS_BUF - 1)
            file_buf[p++] = '\n';
    }
    file_buf[p] = 0;
    memset(hex, 0, sizeof(hex));

    if (rvfs_create(ACCOUNTS_PATH) < 0)
        return -1;
    if (rvfs_write(ACCOUNTS_PATH, 0, file_buf, (u32)p) != p)
        return -1;
    rvfs_sync();

    copy_limited(current_user, clean, sizeof(current_user));
    refresh();
    return 0;
}
