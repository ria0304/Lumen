#ifndef USERS_H
#define USERS_H

#include <stdint.h>

#define MAX_USERS 8
#define USER_NAME_MAX 20
#define USER_PASS_MAX 20
#define USER_HASH_HEX 64
#define USER_SALT_HEX 16
#define USER_RECORD_MAX 128
#define USERS_PASSWD_PATH "/etc/passwd"

int users_init(void);
int user_add(const char *name, const char *pass, uint16_t uid);
int user_auth(const char *name, const char *pass);
int user_login(const char *name, const char *pass);
void user_logout(void);
const char *user_current(void);
void users_list(void);
int users_save(void);
int users_load(void);
int users_run_self_test(void);

#endif
