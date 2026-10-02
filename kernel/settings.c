#include "settings.h"
#include "fs.h"
#include "console.h"
#include "heap.h"
typedef struct { char key[SETTINGS_KEY_LEN]; char val[SETTINGS_VAL_LEN]; } setting_t;
static setting_t s_table[SETTINGS_MAX_KEYS];
static uint32_t s_count = 0;
static void scopy(char *d, const char *s, uint32_t n) {
    uint32_t i = 0; while (s[i] && i + 1 < n) { d[i] = s[i]; i++; } d[i] = 0;
}
static int scmp(const char *a, const char *b) {
    int i = 0; while (a[i] && a[i] == b[i]) i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}
static void set_default(const char *k, const char *v) {
    if (s_count >= SETTINGS_MAX_KEYS) return;
    scopy(s_table[s_count].key, k, SETTINGS_KEY_LEN);
    scopy(s_table[s_count].val, v, SETTINGS_VAL_LEN);
    s_count++;
}
int settings_init(void) {
    s_count = 0;
    set_default("hostname", "lumen");
    set_default("timezone", "UTC0");
    set_default("theme", "dark");
    set_default("kbd_layout", "us");
    set_default("display_mode", "text80x25");
    set_default("boot_splash", "1");
    set_default("sleep_timeout", "300");
    set_default("admin_user", "root");
    settings_load();
    return 0;
}
int settings_get(const char *key, char *out, uint32_t out_size) {
    for (uint32_t i = 0; i < s_count; i++)
        if (scmp(s_table[i].key, key) == 0) { scopy(out, s_table[i].val, out_size); return 0; }
    return -1;
}
int settings_set(const char *key, const char *val) {
    for (uint32_t i = 0; i < s_count; i++)
        if (scmp(s_table[i].key, key) == 0) { scopy(s_table[i].val, val, SETTINGS_VAL_LEN); return 0; }
    if (s_count >= SETTINGS_MAX_KEYS) return -1;
    scopy(s_table[s_count].key, key, SETTINGS_KEY_LEN);
    scopy(s_table[s_count].val, val, SETTINGS_VAL_LEN);
    s_count++;
    return 0;
}
int settings_save(void) {
    if (!fs_is_mounted()) return -1;
    char *buf = (char *)kmalloc(2048);
    if (!buf) return -1;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < s_count && pos + 80 < 2048; i++) {
        uint32_t j = 0;
        while (s_table[i].key[j] && pos + 1 < 2048) buf[pos++] = s_table[i].key[j++];
        buf[pos++] = '=';
        j = 0;
        while (s_table[i].val[j] && pos + 1 < 2048) buf[pos++] = s_table[i].val[j++];
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    fs_mkdir("/etc", FS_ROOT, FS_MODE_DIR_DEFAULT);
    int rc = fs_write("/etc/settings", FS_ROOT, buf, pos);
    kfree(buf);
    return rc == FS_OK ? 0 : -1;
}
int settings_load(void) {
    if (!fs_is_mounted()) return -1;
    void *data = 0; uint32_t size = 0;
    if (fs_read("/etc/settings", FS_ROOT, &data, &size) != FS_OK) return -1;
    char *p = (char *)data;
    char key[SETTINGS_KEY_LEN], val[SETTINGS_VAL_LEN];
    uint32_t i = 0;
    while (i < size) {
        uint32_t k = 0;
        while (i < size && p[i] != '=' && p[i] != '\n' && k + 1 < SETTINGS_KEY_LEN) key[k++] = p[i++];
        key[k] = 0;
        if (i < size && p[i] == '=') i++;
        uint32_t v = 0;
        while (i < size && p[i] != '\n' && v + 1 < SETTINGS_VAL_LEN) val[v++] = p[i++];
        val[v] = 0;
        if (i < size && p[i] == '\n') i++;
        if (k > 0) settings_set(key, val);
    }
    kfree(data);
    return 0;
}
void settings_list(void) {
    for (uint32_t i = 0; i < s_count; i++) {
        terminal_write(s_table[i].key);
        terminal_write("=");
        terminal_write(s_table[i].val);
        terminal_putchar('\n');
    }
}
int settings_run_self_test(void) {
    settings_set("selftest_key", "42");
    char tmp[SETTINGS_VAL_LEN];
    if (settings_get("selftest_key", tmp, sizeof(tmp)) != 0) return 0;
    return (tmp[0] == '4' && tmp[1] == '2');
}
