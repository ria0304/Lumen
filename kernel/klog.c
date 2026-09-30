#include "klog.h"
#include "console.h"
#define KLOG_SIZE 1024
static char kbuf[KLOG_SIZE];
static uint32_t kpos = 0;
void klog_put(const char *msg) {
    int i = 0;
    while (msg[i] && kpos + 1 < KLOG_SIZE) kbuf[kpos++] = msg[i++];
    if (kpos + 1 < KLOG_SIZE) kbuf[kpos++] = '\n';
}
void klog_dump(void) {
    for (uint32_t i = 0; i < kpos; i++) terminal_putchar(kbuf[i]);
}
int klog_run_self_test(void) {
    klog_put("klog selftest");
    return kpos > 0;
}
