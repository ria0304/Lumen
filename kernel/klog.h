#ifndef KLOG_H
#define KLOG_H
#include <stdint.h>
void klog_put(const char *msg);
void klog_dump(void);
int klog_run_self_test(void);
#endif
