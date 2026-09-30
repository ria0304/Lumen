#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdint.h>
#define SETTINGS_MAX_KEYS 32
#define SETTINGS_KEY_LEN 24
#define SETTINGS_VAL_LEN 48
int settings_init(void);
int settings_get(const char *key, char *out, uint32_t out_size);
int settings_set(const char *key, const char *val);
int settings_save(void);
int settings_load(void);
void settings_list(void);
int settings_run_self_test(void);
#endif
