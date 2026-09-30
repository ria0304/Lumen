/* RTL8139 PCI driver (polling mode) + ARP/IPv4/ICMP helpers.
 * No interrupts wired: TX/RX are polled via ISR register.
 * Own IP is 10.0.2.15 (QEMU user-net default); loopback stays in net.c. */
#include "nic.h"
#include "frame.h"
#include "console.h"

extern volatile uint32_t timer_ticks;

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define R_RX_OK 0x01
#define R_RX_ERR 0x02
#define R_TX_OK 0x04
#define RX_BUF_SIZE 8192

static inline void outb(uint16_t p, uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void outw(uint16_t p, uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline void outl(uint16_t p, uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint16_t inw(uint16_t p){uint16_t r;__asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint32_t inl(uint16_t p){uint32_t r;__asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p));return r;}

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off){
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)slot<<11) | ((uint32_t)func<<8) | (off & 0xFCu));
    return inl(PCI_DATA);
}

static int have_hw = 0;
static uint16_t iob = 0;
static uint8_t mac[6];
static uint32_t rx_phys = 0, tx_phys = 0;
static uint8_t *rx_buf = 0, *tx_buf = 0;
static uint16_t rx_off = 0;
static int tx_idx = 0;
static uint32_t txc = 0, rxc = 0;

/* ARP table */
#define ARP_N 8
static struct { uint32_t ip; uint8_t mac[6]; int used; } arp[ARP_N];

uint16_t ip_checksum(const void *data, uint32_t len){
    const uint8_t *p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint32_t)p[0]<<8)|p[1]; p+=2; len-=2; }
    if (len) sum += ((uint32_t)p[0]<<8);
    while (sum>>16) sum = (sum&0xFFFF)+(sum>>16);
    return (uint16_t)~sum;
}

int arp_lookup(uint32_t ip, uint8_t *mac_out){
    for(int i=0;i<ARP_N;i++) if(arp[i].used && arp[i].ip==ip){
        for(int k=0;k<6;k++)mac_out[k]=arp[i].mac[k]; return 0;
    }
    return -1;
}
void arp_add(uint32_t ip, const uint8_t *m){
    for(int i=0;i<ARP_N;i++) if(arp[i].used && arp[i].ip==ip){
        for(int k=0;k<6;k++)arp[i].mac[k]=m[k]; return;
    }
    for(int i=0;i<ARP_N;i++) if(!arp[i].used){
        arp[i].ip=ip; for(int k=0;k<6;k++)arp[i].mac[k]=m[k]; arp[i].used=1; return;
    }
    arp[0].ip=ip; for(int k=0;k<6;k++)arp[0].mac[k]=m[k]; arp[0].used=1;
}

static int low_frames(uint32_t *a, uint32_t *b, uint32_t *c){
    /* collect 3 sub-4MiB frames (identity-mapped, DMA-safe) */
    uint32_t got[12]; int n=0, ok=0;
    uint32_t ra=0,rb=0,rc=0;
    for(int i=0;i<12 && !ok;i++){
        uint32_t f = frame_alloc();
        if(f==FRAME_INVALID) break;
        got[n++]=f;
        if(f<0x400000){ if(!ra)ra=f; else if(!rb)rb=f; else if(!rc){rc=f;ok=1;} }
    }
    for(int i=0;i<n;i++){ uint32_t f=got[i]; if(f!=ra&&f!=rb&&f!=rc) frame_free(f); }
    if(!ok){ if(ra)frame_free(ra); if(rb)frame_free(rb); if(rc)frame_free(rc); return -1; }
    *a=ra;*b=rb;*c=rc; return 0;
}

int nic_init(void){
    for(int s=0;s<32 && !have_hw;s++){
        uint32_t id = pci_read(0,(uint8_t)s,0,0x00);
        uint16_t ven = id & 0xFFFF, dev = (id>>16) & 0xFFFF;
        if(ven==0x10EC && dev==0x8139){
            uint32_t bar = pci_read(0,(uint8_t)s,0,0x10);
            iob = (uint16_t)(bar & 0xFFFCu);
            if(iob==0) continue;
            for(int k=0;k<6;k++) mac[k]=inb(iob+k);
            uint32_t ra,rb,rc;
            if(low_frames(&ra,&rb,&rc)!=0) continue; /* no DMA mem: skip */
            rx_phys=ra; tx_phys=rb; frame_free(rc);
            rx_buf=(uint8_t*)rx_phys; tx_buf=(uint8_t*)tx_phys;
            outb(iob+0x52, 0x00);            /* power on */
            outb(iob+0x37, 0x10);            /* reset */
            uint32_t t=timer_ticks;
            while((inb(iob+0x37)&0x10) && timer_ticks-t<100) {}
            outl(iob+0x30, rx_phys);         /* RBSTART */
            outl(iob+0x44, 0x0F);            /* accept AB/AM/APM/AAP */
            outb(iob+0x37, 0x0C);            /* RE|TE */
            rx_off=0; have_hw=1;
        }
    }
    return 0;
}
int nic_present(void){return have_hw;}
const uint8_t *nic_mac(void){return mac;}
uint16_t nic_iobase(void){return iob;}
uint32_t nic_tx_count(void){return txc;}
uint32_t nic_rx_count(void){return rxc;}

int nic_send(const void *frame, uint32_t len){
    if(!have_hw || !frame || len<14 || len>1536) return -1;
    const uint8_t *p=(const uint8_t*)frame;
    for(uint32_t i=0;i<len;i++) tx_buf[i]=p[i];
    int d = tx_idx; tx_idx=(tx_idx+1)&3;
    outl((uint16_t)(iob+0x20+d*4), tx_phys);
    outl((uint16_t)(iob+0x10+d*4), len);
    uint32_t t=timer_ticks;
    while(timer_ticks-t<50){
        uint16_t isr=inw(iob+0x3E);
        if(isr & R_TX_OK){ outw(iob+0x3E, R_TX_OK); txc++; return (int)len; }
        if(isr & 0x08){ outw(iob+0x3E, 0x08); return -1; }
    }
    return -1;
}

/* drain one RX packet into out; 0 = none available */
int nic_poll_rx(void){ return 0; }

int nic_recv(void *out, uint32_t max, uint32_t timeout_ticks){
    if(!have_hw) return -1;
    uint8_t *o=(uint8_t*)out;
    uint32_t start=timer_ticks;
    while(timer_ticks-start<timeout_ticks){
        uint16_t isr=inw(iob+0x3E);
        if(!(isr & (R_RX_OK|R_RX_ERR))) continue;
        uint16_t sts = rx_buf[rx_off] | ((uint16_t)rx_buf[(rx_off+1)%RX_BUF_SIZE]<<8);
        uint16_t len = rx_buf[(rx_off+2)%RX_BUF_SIZE] | ((uint16_t)rx_buf[(rx_off+3)%RX_BUF_SIZE]<<8);
        uint16_t next = (uint16_t)((rx_off + len + 4 + 3) & ~3);
        if(isr & R_RX_OK){
            uint32_t copy = len>4?len-4:0;
            if(copy>max)copy=max;
            for(uint32_t i=0;i<copy;i++) o[i]=rx_buf[(rx_off+4+i)%RX_BUF_SIZE];
            (void)sts;
            rx_off = next>=RX_BUF_SIZE ? (uint16_t)(next-RX_BUF_SIZE) : next;
            outw(iob+0x38, (uint16_t)(rx_off-0x10));
            outw(iob+0x3E, R_RX_OK);
            rxc++;
            return (int)copy;
        } else {
            rx_off = next>=RX_BUF_SIZE ? (uint16_t)(next-RX_BUF_SIZE) : next;
            outw(iob+0x38, (uint16_t)(rx_off-0x10));
            outw(iob+0x3E, R_RX_ERR);
        }
    }
    return 0;
}

static void eth_ip_icmp(uint8_t *f, const uint8_t *dst, uint32_t sip, uint32_t dip,
                        uint16_t id, uint16_t seq, uint32_t *out_len){
    for(int i=0;i<6;i++){f[i]=dst[i];f[6+i]=mac[i];}
    f[12]=0x08;f[13]=0x00;
    uint8_t *ip=f+14;
    ip[0]=0x45;ip[1]=0;ip[2]=0;ip[3]=28;ip[4]=id>>8;ip[5]=id&0xFF;ip[6]=0x40;ip[7]=0;
    ip[8]=64;ip[9]=1;ip[10]=0;ip[11]=0;
    ip[12]=sip>>24;ip[13]=(sip>>16)&0xFF;ip[14]=(sip>>8)&0xFF;ip[15]=sip&0xFF;
    ip[16]=dip>>24;ip[17]=(dip>>16)&0xFF;ip[18]=(dip>>8)&0xFF;ip[19]=dip&0xFF;
    uint16_t c=ip_checksum(ip,20);ip[10]=c>>8;ip[11]=c&0xFF;
    uint8_t *ic=ip+20;
    ic[0]=8;ic[1]=0;ic[2]=0;ic[3]=0;ic[4]=id>>8;ic[5]=id&0xFF;ic[6]=seq>>8;ic[7]=seq&0xFF;
    c=ip_checksum(ic,8);ic[2]=c>>8;ic[3]=c&0xFF;
    *out_len=42;
}

static int arp_request(uint32_t tip, uint8_t *mac_out){
    static const uint8_t bcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t f[64]; uint8_t *p=f;
    for(int i=0;i<6;i++)*p++=bcast[i];
    for(int i=0;i<6;i++)*p++=mac[i];
    *p++=0x08;*p++=0x06;
    *p++=0x00;*p++=0x01;*p++=0x08;*p++=0x00;*p++=0x06;*p++=0x04;
    *p++=0x00;*p++=0x01;
    for(int i=0;i<6;i++)*p++=mac[i];
    uint32_t sip=0x0A00020Fu;
    *p++=sip>>24;*p++=(sip>>16)&0xFF;*p++=(sip>>8)&0xFF;*p++=sip&0xFF;
    for(int i=0;i<6;i++)*p++=0;
    *p++=tip>>24;*p++=(tip>>16)&0xFF;*p++=(tip>>8)&0xFF;*p++=tip&0xFF;
    if(nic_send(f,42)<0) return -1;
    uint8_t rx[256];
    uint32_t start=timer_ticks;
    while(timer_ticks-start<200){
        int n=nic_recv(rx,sizeof(rx),10);
        if(n>=42 && rx[12]==0x08 && rx[13]==0x06 && rx[20]==0x00 && rx[21]==0x02){
            uint32_t sip2=((uint32_t)rx[28]<<24)|((uint32_t)rx[29]<<16)|((uint32_t)rx[30]<<8)|rx[31];
            if(sip2==tip){ for(int i=0;i<6;i++)mac_out[i]=rx[22+i]; arp_add(tip,mac_out); return 0; }
        }
        /* learn gratuitous ARPs while waiting */
        if(n>=42 && rx[12]==0x08 && rx[13]==0x06){
            uint32_t s2=((uint32_t)rx[28]<<24)|((uint32_t)rx[29]<<16)|((uint32_t)rx[30]<<8)|rx[31];
            uint8_t m2[6]; for(int i=0;i<6;i++)m2[i]=rx[22+i]; arp_add(s2,m2);
        }
    }
    return -1;
}

int nic_ping_ip(uint32_t dip){
    if(!have_hw) return -1;
    uint8_t dst[6];
    if(arp_lookup(dip,dst)!=0 && arp_request(dip,dst)!=0) return -2; /* no route */
    uint8_t f[64]; uint32_t flen=0;
    uint32_t sip=0x0A00020Fu;
    static uint16_t seq=0;
    eth_ip_icmp(f,dst,sip,dip,0x1234,++seq,&flen);
    if(nic_send(f,flen)<0) return -1;
    uint8_t rx[256];
    uint32_t start=timer_ticks;
    while(timer_ticks-start<200){
        int n=nic_recv(rx,sizeof(rx),10);
        if(n>=42 && rx[12]==0x08 && rx[13]==0x00 && rx[23]==1 && rx[34]==0){
            uint32_t s2=((uint32_t)rx[26]<<24)|((uint32_t)rx[27]<<16)|((uint32_t)rx[28]<<8)|rx[29];
            if(s2==sip) return 0;
        }
    }
    return -3; /* sent, no reply */
}

void nic_dump(void){
    if(!have_hw){ terminal_write("nic: none (loopback only)\n"); return; }
    terminal_write("nic: rtl8139 io=0x"); terminal_write_u32(iob);
    terminal_write(" mac=");
    for(int i=0;i<6;i++){ terminal_write_hex8(mac[i]); if(i<5)terminal_putchar(':'); }
    terminal_putchar('\n');
    terminal_write("ip=10.0.2.15 tx="); terminal_write_u32(txc);
    terminal_write(" rx="); terminal_write_u32(rxc); terminal_putchar('\n');
    terminal_write("arp:\n");
    for(int i=0;i<ARP_N;i++) if(arp[i].used){
        terminal_write_u32(arp[i].ip>>24);terminal_putchar('.');
        terminal_write_u32((arp[i].ip>>16)&0xFF);terminal_putchar('.');
        terminal_write_u32((arp[i].ip>>8)&0xFF);terminal_putchar('.');
        terminal_write_u32(arp[i].ip&0xFF);terminal_write(" -> ");
        for(int k=0;k<6;k++){terminal_write_hex8(arp[i].mac[k]);if(k<5)terminal_putchar(':');}
        terminal_putchar('\n');
    }
}

int nic_run_self_test(void){
    /* checksum vector: IP header of 20 zero bytes -> 0xFFFF */
    uint8_t z[20]; for(int i=0;i<20;i++)z[i]=0;
    if(ip_checksum(z,20)!=0xFFFF) return 0;
    uint8_t m[6]={1,2,3,4,5,6};
    arp_add(0x0A000202u,m);
    uint8_t o[6];
    if(arp_lookup(0x0A000202u,o)!=0) return 0;
    for(int i=0;i<6;i++) if(o[i]!=m[i]) return 0;
    return 1;
}
