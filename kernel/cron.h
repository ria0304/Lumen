#ifndef CRON_H
#define CRON_H
#include <stdint.h>
int cron_add(const char *cmd, uint32_t interval_ticks);
int cron_remove(int id);
void cron_list(void);
void cron_tick(uint32_t now);
void cron_poll(void);
int cron_run_self_test(void);
#endif
