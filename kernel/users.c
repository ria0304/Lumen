#include "users.h"
#include "console.h"
#include "heap.h"
#include "fs.h"
#define MAX_USERS 8
typedef struct { char name[24]; char pass[24]; uint16_t uid; int used; } user_t;
static user_t u_tab[MAX_USERS];
static int u_current = -1;
static void scopy(char *d, const char *s, int n){int i=0;while(s[i]&&i+1<n){d[i]=s[i];i++;}d[i]=0;}
static int scmp(const char *a,const char *b){int i=0;while(a[i]&&a[i]==b[i])i++;return(unsigned char)a[i]-(unsigned char)b[i];}
int users_init(void){
    for(int i=0;i<MAX_USERS;i++)u_tab[i].used=0;
    scopy(u_tab[0].name,"root",24);scopy(u_tab[0].pass,"root",24);u_tab[0].uid=0;u_tab[0].used=1;
    u_current=0; return 0;
}
int user_add(const char *name,const char *pass,uint16_t uid){
    for(int i=0;i<MAX_USERS;i++)if(u_tab[i].used&&scmp(u_tab[i].name,name)==0)return-1;
    for(int i=0;i<MAX_USERS;i++)if(!u_tab[i].used){scopy(u_tab[i].name,name,24);scopy(u_tab[i].pass,pass,24);u_tab[i].uid=uid;u_tab[i].used=1;return 0;}
    return -1;
}
int user_auth(const char *name,const char *pass){
    for(int i=0;i<MAX_USERS;i++)if(u_tab[i].used&&scmp(u_tab[i].name,name)==0)
        return scmp(u_tab[i].pass,pass)==0?i:-1;
    return -1;
}
int user_login(const char *name,const char *pass){
    int i=user_auth(name,pass); if(i>=0){u_current=i;return 0;} return -1;
}
void user_logout(void){u_current=-1;}
const char *user_current(void){return u_current>=0?u_tab[u_current].name:"(none)";}
void users_list(void){for(int i=0;i<MAX_USERS;i++)if(u_tab[i].used){terminal_write(u_tab[i].name);terminal_putchar('\n');}}
int users_run_self_test(void){
    if(user_add("selftest","pw",1000)!=0)return 0;
    if(user_auth("selftest","pw")<0)return 0;
    if(user_auth("selftest","wrong")>=0)return 0;
    return 1;
}
