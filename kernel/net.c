#include "net.h"
#include "console.h"
#include "nic.h"
#define NET_BUF 512
static uint8_t lb_buf[NET_BUF];
static uint32_t lb_len = 0;
static uint32_t lb_ip = 0x7F000001u;
static uint32_t stat_tx = 0, stat_rx = 0;
int net_init(void){lb_len=0;stat_tx=stat_rx=0;return 0;}
int net_ping(uint32_t ip){
    if((ip>>24)==0x7F){stat_tx++;stat_rx++;return 0;}
    int r=nic_ping_ip(ip);
    if(r==0){stat_tx++;stat_rx++;}
    return r;
}
int net_send(const void *data,uint32_t len){
    const uint8_t *p=(const uint8_t*)data;
    if(len>NET_BUF)len=NET_BUF;
    for(uint32_t i=0;i<len;i++)lb_buf[i]=p[i];
    lb_len=len;stat_tx++;return(int)len;
}
int net_recv(void *out,uint32_t max){
    uint8_t *p=(uint8_t*)out;
    uint32_t n=lb_len<max?lb_len:max;
    for(uint32_t i=0;i<n;i++)p[i]=lb_buf[i];
    lb_len=0;stat_rx++;return(int)n;
}
void net_ifconfig(void){
    terminal_write("lo: 127.0.0.1 up loopback\n");
    nic_dump();
    (void)lb_ip;
}
void net_stat(void){
    terminal_write("tx=");terminal_write_u32(stat_tx);
    terminal_write(" rx=");terminal_write_u32(stat_rx);
    terminal_putchar('\n');
}
int net_run_self_test(void){
    const char m[]="hello";
    if(net_send(m,5)!=5)return 0;
    char b[8]; if(net_recv(b,8)!=5)return 0;
    return b[0]=='h'&&b[4]=='o';
}
