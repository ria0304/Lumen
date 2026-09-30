#ifndef PKG_H
#define PKG_H
#include <stdint.h>
int pkg_init(void);
int pkg_install(const char *name, const char *ver);
int pkg_remove(const char *name);
void pkg_list(void);
int pkg_run_self_test(void);
#endif
