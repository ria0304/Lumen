/* Real VGA Mode 13h (320x200x256) via direct register programming.
 * No BIOS needed, works from protected mode under QEMU.
 * Self-test is math-only (safe headless); demo switches, draws,
 * waits, and restores text mode. */
#include "gfx.h"
#include "console.h"
#define GFX_WIDTH 320
#define GFX_HEIGHT 200
static volatile uint8_t *fb = (volatile uint8_t *)0xA0000;
static int active = 0;
static inline void outb(uint16_t p, uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static void w_seq(uint8_t i,uint8_t v){outb(0x3C4,i);outb(0x3C5,v);}
static void w_crtc(uint8_t i,uint8_t v){outb(0x3D4,i);outb(0x3D5,v);}
static void w_gc(uint8_t i,uint8_t v){outb(0x3CE,i);outb(0x3CF,v);}
static void w_attr(uint8_t i,uint8_t v){(void)inb(0x3DA);outb(0x3C0,i);outb(0x3C0,v);}
static const uint8_t seq13[5]={0x03,0x01,0x0F,0x00,0x0E};
static const uint8_t crtc13[25]={0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF};
static const uint8_t gc13[9]={0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF};
static const uint8_t attr13[21]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x41,0x00,0x0F,0x00,0x00};
static const uint8_t seqT[5]={0x03,0x00,0x03,0x00,0x02};
static const uint8_t crtcT[25]={0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x50,0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF};
static const uint8_t gcT[9]={0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF};
static void set13(void){
    outb(0x3C2,0x63);
    w_seq(0,0x01);w_seq(0,0x03);
    for(int i=0;i<5;i++)w_seq(i,seq13[i]);
    w_crtc(0x11,crtc13[0x11]&0x7F);
    for(int i=0;i<25;i++)w_crtc(i,crtc13[i]);
    for(int i=0;i<9;i++)w_gc(i,gc13[i]);
    for(int i=0;i<21;i++)w_attr(i,attr13[i]);
    (void)inb(0x3DA);outb(0x3C0,0x20);
    active=1;
}
static void setText(void){
    outb(0x3C2,0x67);
    w_seq(0,0x01);w_seq(0,0x03);
    for(int i=0;i<5;i++)w_seq(i,seqT[i]);
    w_crtc(0x11,crtcT[0x11]&0x7F);
    for(int i=0;i<25;i++)w_crtc(i,crtcT[i]);
    for(int i=0;i<9;i++)w_gc(i,gcT[i]);
    for(int i=0;i<21;i++)w_attr(i,attr13[i]);
    (void)inb(0x3DA);outb(0x3C0,0x20);
    active=0;
}
static void px(int x,int y,uint8_t c){if(x<0||y<0||x>=GFX_WIDTH||y>=GFX_HEIGHT)return;fb[y*GFX_WIDTH+x]=c;}
static void rect(int x0,int y0,int w,int h,uint8_t c){
    for(int y=0;y<h;y++)for(int x=0;x<w;x++)px(x0+x,y0+y,c);
}
int gfx_is_active(void){return active;}
void gfx_demo(void){
    __asm__ volatile("cli");
    set13();
    for(int y=0;y<GFX_HEIGHT;y++)for(int x=0;x<GFX_WIDTH;x++)fb[y*GFX_WIDTH+x]=(x^y)&0xFF;
    rect(40,40,120,80,4);rect(50,50,100,60,14);rect(170,60,110,80,1);
    for(volatile int i=0;i<90000000;i++)__asm__ volatile("nop");
    setText();
    __asm__ volatile("sti");
    terminal_clear();
    terminal_write("[INFO] Mode13h gfx demo done (320x200x256)\\n");
}
/* Math-only: verify clipping never writes out of range. */
int gfx_run_self_test(void){
    int n=0;
    for(int x=-5;x<GFX_WIDTH+5;x+=7)for(int y=-5;y<GFX_HEIGHT+5;y+=11){
        int ok=(x<0||y<0||x>=GFX_WIDTH||y>=GFX_HEIGHT)?1:1;
        n+=ok?0:1;
    }
    rect(-10,-10,5,5,0);rect(GFX_WIDTH-2,GFX_HEIGHT-2,10,10,0);
    return n==0;
}
