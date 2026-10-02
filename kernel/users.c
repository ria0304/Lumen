#include "users.h"
#include "console.h"
#include "heap.h"
#include "fs.h"
#include "sha256.h"
#include "rtc.h"

typedef struct {
    char name[USER_NAME_MAX + 1];
    char salt[USER_SALT_HEX + 1];
    char hash[USER_HASH_HEX + 1];
    uint16_t uid;
    int used;
} user_t;

static user_t u_tab[MAX_USERS];
static int u_current = -1;

static void scopy(char *d, const char *s, int n)
{
    int i = 0;
    while (s[i] && i + 1 < n) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int scmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static void bytes_to_hex(const uint8_t *bytes, int len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < len; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    out[len * 2] = 0;
}

static void generate_salt(char *out)
{
    rtc_time_t t;
    uint8_t raw[8];
    uint8_t digest[SHA256_DIGEST_SIZE];
    int i;

    if (rtc_read(&t) == 0) {
        raw[0] = (uint8_t)(t.second);
        raw[1] = (uint8_t)(t.minute);
        raw[2] = (uint8_t)(t.hour);
        raw[3] = (uint8_t)(t.day);
        raw[4] = (uint8_t)(t.month);
        raw[5] = (uint8_t)(t.year);
        raw[6] = (uint8_t)(t.year >> 8);
        raw[7] = (uint8_t)(t.weekday);
    } else {
        for (i = 0; i < 8; i++)
            raw[i] = (uint8_t)(i * 37 + 11);
    }

    /* The digest needs its own 32-byte buffer: sha256 always writes
     * SHA256_DIGEST_SIZE bytes, so hashing in place over an 8-byte
     * input smashed the stack. 'out' is only USER_SALT_HEX + 1 bytes,
     * so keep taking the first 8 digest bytes to preserve the on-disk
     * 16-hex-char salt format. */
    sha256(raw, 8, digest);
    bytes_to_hex(digest, 8, out);
}

static void hash_password(const char *salt_hex, const char *pass, char *out)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;
    int salt_len = 0;
    int pass_len = 0;

    while (salt_hex[salt_len])
        salt_len++;
    while (pass[pass_len])
        pass_len++;

    sha256_init(&ctx);
    sha256_update(&ctx, salt_hex, (uint32_t)salt_len);
    sha256_update(&ctx, ":", 1);
    sha256_update(&ctx, pass, (uint32_t)pass_len);
    sha256_final(&ctx, digest);
    bytes_to_hex(digest, SHA256_DIGEST_SIZE, out);
}

static int find_user(const char *name)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (u_tab[i].used && scmp(u_tab[i].name, name) == 0)
            return i;
    }
    return -1;
}

int users_init(void)
{
    for (int i = 0; i < MAX_USERS; i++)
        u_tab[i].used = 0;

    u_current = -1;

    if (users_load() != 0) {
        /* First boot or no persisted file: create root with a
         * default password so the system is usable. */
        user_add("root", "root", 0);
        users_save();
        u_current = 0;
    }

    return 0;
}

int user_add(const char *name, const char *pass, uint16_t uid)
{
    int slot;

    if (!name || !pass || !name[0] || !pass[0])
        return -1;

    if (find_user(name) >= 0)
        return -1;

    for (slot = 0; slot < MAX_USERS; slot++) {
        if (!u_tab[slot].used)
            break;
    }

    if (slot >= MAX_USERS)
        return -1;

    scopy(u_tab[slot].name, name, USER_NAME_MAX + 1);
    generate_salt(u_tab[slot].salt);
    hash_password(u_tab[slot].salt, pass, u_tab[slot].hash);
    u_tab[slot].uid = uid;
    u_tab[slot].used = 1;

    return 0;
}

int user_auth(const char *name, const char *pass)
{
    int i = find_user(name);
    char expected[USER_HASH_HEX + 1];

    if (i < 0)
        return -1;

    hash_password(u_tab[i].salt, pass, expected);

    if (scmp(u_tab[i].hash, expected) == 0)
        return i;

    return -1;
}

int user_login(const char *name, const char *pass)
{
    int i = user_auth(name, pass);
    if (i >= 0) {
        u_current = i;
        return 0;
    }
    return -1;
}

void user_logout(void)
{
    u_current = -1;
}

const char *user_current(void)
{
    return u_current >= 0 ? u_tab[u_current].name : "(none)";
}

void users_list(void)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (u_tab[i].used) {
            terminal_write(u_tab[i].name);
            terminal_write(":");
            terminal_write_u32((uint32_t)u_tab[i].uid);
            terminal_putchar('\n');
        }
    }
}

int users_save(void)
{
    static char buf[1024];
    uint32_t pos = 0;

    if (!fs_is_mounted())
        return -1;

    fs_mkdir("/etc", FS_ROOT, FS_MODE_DIR_DEFAULT);

    for (int i = 0; i < MAX_USERS; i++) {
        if (!u_tab[i].used)
            continue;

        uint32_t j = 0;
        while (u_tab[i].name[j] && pos + 1 < sizeof(buf))
            buf[pos++] = u_tab[i].name[j++];
        if (pos + 1 >= sizeof(buf)) return -1;
        buf[pos++] = ':';
        j = 0;
        while (u_tab[i].salt[j] && pos + 1 < sizeof(buf))
            buf[pos++] = u_tab[i].salt[j++];
        if (pos + 1 >= sizeof(buf)) return -1;
        buf[pos++] = ':';
        j = 0;
        while (u_tab[i].hash[j] && pos + 1 < sizeof(buf))
            buf[pos++] = u_tab[i].hash[j++];
        if (pos + 1 >= sizeof(buf)) return -1;
        buf[pos++] = ':';
        if (pos + 6 >= sizeof(buf)) return -1;
        buf[pos++] = '0' + (char)(u_tab[i].uid / 1000 % 10);
        buf[pos++] = '0' + (char)(u_tab[i].uid / 100 % 10);
        buf[pos++] = '0' + (char)(u_tab[i].uid / 10 % 10);
        buf[pos++] = '0' + (char)(u_tab[i].uid % 10);
        buf[pos++] = '\n';
    }

    buf[pos] = 0;

    return fs_write(USERS_PASSWD_PATH, FS_ROOT, buf, pos) == FS_OK ? 0 : -1;
}

int users_load(void)
{
    void *data = 0;
    uint32_t size = 0;
    char *p;
    int count = 0;

    if (!fs_is_mounted())
        return -1;

    if (fs_read(USERS_PASSWD_PATH, FS_ROOT, &data, &size) != FS_OK)
        return -1;

    p = (char *)data;

    while (size > 0) {
        char name[USER_NAME_MAX + 1];
        char salt[USER_SALT_HEX + 1];
        char hash[USER_HASH_HEX + 1];
        uint16_t uid = 0;
        uint32_t k;

        k = 0;
        while (size > 0 && *p != ':' && k < USER_NAME_MAX)
            name[k++] = *p++, size--;
        name[k] = 0;
        if (size > 0)
            p++, size--;

        k = 0;
        while (size > 0 && *p != ':' && k < USER_SALT_HEX)
            salt[k++] = *p++, size--;
        salt[k] = 0;
        if (size > 0)
            p++, size--;

        k = 0;
        while (size > 0 && *p != ':' && k < USER_HASH_HEX)
            hash[k++] = *p++, size--;
        hash[k] = 0;
        if (size > 0)
            p++, size--;

        while (size > 0 && *p >= '0' && *p <= '9')
            uid = (uint16_t)(uid * 10 + (uint16_t)(*p++ - '0')), size--;
        if (size > 0)
            p++, size--;

        if (name[0] && salt[0] && hash[0]) {
            for (int i = 0; i < MAX_USERS; i++) {
                if (!u_tab[i].used) {
                    scopy(u_tab[i].name, name, USER_NAME_MAX + 1);
                    scopy(u_tab[i].salt, salt, USER_SALT_HEX + 1);
                    scopy(u_tab[i].hash, hash, USER_HASH_HEX + 1);
                    u_tab[i].uid = uid;
                    u_tab[i].used = 1;
                    count++;
                    break;
                }
            }
        }
    }

    kfree(data);

    return count > 0 ? 0 : -1;
}

int users_run_self_test(void)
{
    uint8_t d1[SHA256_DIGEST_SIZE];
    uint8_t d2[SHA256_DIGEST_SIZE];
    int failures = 0;

    /* Name each check so a failure is identifiable from the boot log
     * instead of collapsing into one bare "SELFTEST USERS FAIL". */
#define UCHECK(cond, msg)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("USERS self-test: " msg);                        \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    /* SHA-256("abc") known vector. */
    {
        const char *abc = "abc";
        sha256(abc, 3, d1);
    }

    {
        const uint8_t expect[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        int match = 1;
        for (int i = 0; i < 32; i++) {
            if (d1[i] != expect[i])
                match = 0;
        }
        UCHECK(match, "SHA-256 known-answer vector mismatch");
    }

    /* Same input, same hash. */
    {
        const char *abc = "abc";
        sha256(abc, 3, d2);
        int same = 1;
        for (int i = 0; i < 32; i++) {
            if (d1[i] != d2[i])
                same = 0;
        }
        UCHECK(same, "SHA-256 is not deterministic");
    }

    /* Different input, different hash. */
    {
        const char *abd = "abd";
        sha256(abd, 3, d2);
        int differs = 0;
        for (int i = 0; i < 32; i++) {
            if (d1[i] != d2[i])
                differs = 1;
        }
        UCHECK(differs, "different inputs produced the same digest");
    }

    /* Add + auth success. */
    UCHECK(user_add("selftest", "pw", 1000) == 0, "user_add failed");
    UCHECK(user_auth("selftest", "pw") >= 0, "correct password rejected");

    /* Auth failure: wrong password. */
    UCHECK(user_auth("selftest", "wrong") < 0, "wrong password accepted");

    /* Auth failure: unknown user. */
    UCHECK(user_auth("nosuchuser", "pw") < 0, "unknown user authenticated");

    /* Duplicate add rejected. */
    UCHECK(user_add("selftest", "other", 1001) != 0, "duplicate user added");

    /* Persistence: save, clear, load, re-auth. */
    if (fs_is_mounted()) {
        UCHECK(users_save() == 0, "users_save failed");

        for (int i = 0; i < MAX_USERS; i++)
            u_tab[i].used = 0;

        UCHECK(users_load() == 0, "users_load failed");
        UCHECK(user_auth("selftest", "pw") >= 0,
               "user did not survive a save/load round trip");
    } else {
        console_warn("USERS self-test: no filesystem, skipping persistence");
    }

#undef UCHECK

    return failures == 0;
}
