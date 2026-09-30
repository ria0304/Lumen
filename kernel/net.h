#ifndef NET_H
#define NET_H
#include <stdint.h>
int net_init(void);
int net_ping(uint32_t ip);
int net_send(const void *data, uint32_t len);
int net_recv(void *out, uint32_t max);
void net_ifconfig(void);
void net_stat(void);
int net_run_self_test(void);
#endif
