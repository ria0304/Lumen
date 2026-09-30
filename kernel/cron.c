#include "cron.h"
#include "console.h"
#include "shell.h"
#define MAX_JOBS 8
typedef struct { char cmd[64]; uint32_t interval; uint32_t next; int due; int used; } job_t;
static job_t jobs[MAX_JOBS];
static void scopy(char *d, const char *s, int n){int i=0;while(s[i]&&i+1<n){d[i]=s[i];i++;}d[i]=0;}
int cron_add(const char *cmd, uint32_t interval_ticks) {
    if (!cmd || !*cmd || interval_ticks == 0) return -1;
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) {
        scopy(jobs[i].cmd, cmd, 64);
        jobs[i].interval = interval_ticks; jobs[i].next = 0;
        jobs[i].due = 0; jobs[i].used = 1; return i;
    }
    return -1;
}
int cron_remove(int id) {
    if (id < 0 || id >= MAX_JOBS || !jobs[id].used) return -1;
    jobs[id].used = 0; return 0;
}
void cron_list(void) {
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) {
        terminal_write_u32((uint32_t)i); terminal_write(": every ");
        terminal_write_u32(jobs[i].interval); terminal_write(" ticks: ");
        terminal_write(jobs[i].cmd); terminal_putchar('\n');
    }
}
/* IRQ context: only mark due, never run shell here. */
void cron_tick(uint32_t now) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        if (jobs[i].next == 0) jobs[i].next = now + jobs[i].interval;
        if (now >= jobs[i].next) { jobs[i].due = 1; jobs[i].next = now + jobs[i].interval; }
    }
}
/* Process context: run due jobs. Called at top of shell_handle_line. */
void cron_poll(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].used && jobs[i].due) {
            jobs[i].due = 0;
            terminal_write("[cron] "); terminal_write(jobs[i].cmd); terminal_putchar('\n');
            shell_handle_line(jobs[i].cmd);
        }
    }
}
int cron_run_self_test(void) {
    int id = cron_add("version", 100);
    if (id < 0) return 0;
    cron_tick(1000);  /* arms next=1100 */
    cron_tick(1200);  /* fires due=1 */
    int ok = jobs[id].due == 1;
    jobs[id].due = 0; cron_remove(id);
    return ok;
}
