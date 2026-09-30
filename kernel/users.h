#ifndef USERS_H
#define USERS_H
#include <stdint.h>
int users_init(void);
int user_add(const char *name, const char *pass, uint16_t uid);
int user_auth(const char *name, const char *pass);
int user_login(const char *name, const char *pass);
void user_logout(void);
const char *user_current(void);
void users_list(void);
int users_run_self_test(void);
#endif
