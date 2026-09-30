#include "pkg.h"
#include "console.h"
#define MAX_PKGS 16
typedef struct{char name[32];char ver[16];int used;}pkg_t;
static pkg_t pkgs[MAX_PKGS];
static void scopy(char*d,const char*s,int n){int i=0;while(s[i]&&i+1<n){d[i]=s[i];i++;}d[i]=0;}
static int scmp(const char*a,const char*b){int i=0;while(a[i]&&a[i]==b[i])i++;return(unsigned char)a[i]-(unsigned char)b[i];}
int pkg_init(void){for(int i=0;i<MAX_PKGS;i++)pkgs[i].used=0;return 0;}
int pkg_install(const char *name,const char *ver){
    for(int i=0;i<MAX_PKGS;i++)if(pkgs[i].used&&scmp(pkgs[i].name,name)==0){scopy(pkgs[i].ver,ver,16);return 0;}
    for(int i=0;i<MAX_PKGS;i++)if(!pkgs[i].used){scopy(pkgs[i].name,name,32);scopy(pkgs[i].ver,ver,16);pkgs[i].used=1;return 0;}
    return -1;
}
int pkg_remove(const char *name){
    for(int i=0;i<MAX_PKGS;i++)if(pkgs[i].used&&scmp(pkgs[i].name,name)==0){pkgs[i].used=0;return 0;}
    return -1;
}
void pkg_list(void){for(int i=0;i<MAX_PKGS;i++)if(pkgs[i].used){terminal_write(pkgs[i].name);terminal_write("-");terminal_write(pkgs[i].ver);terminal_putchar('\n');}}
int pkg_run_self_test(void){
    if(pkg_install("selftest-pkg","1.0")!=0)return 0;
    if(pkg_remove("selftest-pkg")!=0)return 0;
    return 1;
}
