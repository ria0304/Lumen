#include <stdint.h>
#include "shell.h"
#include "version.h"
#include "gui.h"
#include "settings.h"
#include "pkg.h"
#include "klog.h"
#include "gfx.h"
#include "nic.h"
#include "cron.h"
#include "proc.h"
#include "net.h"
#include "users.h"
#include "ring3.h"
#include "console.h"
#include "heap.h"
#include "task.h"
#include "fs.h"
#include "ata.h"
#include "rtc.h"
#include "loader.h"
#include "scheduler.h"


/* Maximum number of commands in a pipeline. */
#define MAX_PIPE_CMDS 8

/* Maximum number of redirections per command. */
#define MAX_REDIRECTIONS 4

/* Redirection types. */
#define REDIR_NONE 0
#define REDIR_IN 1      /* < */
#define REDIR_OUT 2     /* > */
#define REDIR_APPEND 3  /* >> */

/* Shell variables. */
#define MAX_VARS 32
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 256

typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
} shell_var_t;

static shell_var_t shell_vars[MAX_VARS];
static uint32_t var_count = 0;

/* A single command in a pipeline. */
typedef struct {
    char *argv[32];     /* Command arguments */
    int argc;           /* Number of arguments */
    int in_redir;       /* Input redirection type */
    char in_file[128];  /* Input file for redirection */
    int out_redir;      /* Output redirection type */
    char out_file[128]; /* Output file for redirection */
} shell_cmd_t;

void shell_handle_line(const char *line);
static void shell_expand_variables(const char *input, char *output, uint32_t output_size);
static int shell_strcmp(const char *a, const char *b);
static int shell_exec_cmd(shell_cmd_t *cmd, int in_fd, int out_fd);
static int shell_strcmp(const char *a, const char *b) { int i=0; while(a[i]&&a[i]==b[i]) i++; return (unsigned char)a[i]-(unsigned char)b[i]; }
#define SH_PIPEBUF 512
static char sh_pbuf[SH_PIPEBUF]; static uint32_t sh_plen=0; static int sh_cap=0;
static char sh_inbuf[SH_PIPEBUF]; static uint32_t sh_inlen=0; static int sh_have_in=0;
static int sh_last_rc=0;
typedef struct{int pid;char cmd[48];int bg;int used;}sh_job_t;
static sh_job_t sh_jobs[8];
static int sh_bg_next=0;
static int sh_ed_on=0; static char sh_ed_file[128];
static char *sh_ed_buf=0; static uint32_t sh_ed_len=0, sh_ed_cap=0;
static void sh_emit(const char *s, uint32_t n){
    if(sh_cap){for(uint32_t i=0;i<n&&sh_plen+1<SH_PIPEBUF;i++)sh_pbuf[sh_plen++]=s[i];return;}
    for(uint32_t i=0;i<n;i++)terminal_putchar(s[i]);
}
static void sh_emits(const char *s){uint32_t n=0;while(s[n])n++;sh_emit(s,n);}
extern volatile uint32_t timer_ticks;
static int sh_job_add(int pid,const char *cmd,int bg){
    for(int i=0;i<8;i++)if(!sh_jobs[i].used){
        sh_jobs[i].pid=pid;sh_jobs[i].bg=bg;sh_jobs[i].used=1;
        int j=0;while(cmd[j]&&j<47){sh_jobs[i].cmd[j]=cmd[j];j++;}sh_jobs[i].cmd[j]=0;
        return i;
    }return -1;
}


/* Parse a command line into shell_cmd_t structures for pipeline execution.
 * Returns number of commands in pipeline, or -1 on error. */
static int shell_parse_pipeline(const char *line, shell_cmd_t *cmds, int max_cmds)
{
    int cmd_count = 0;
    const char *cursor = line;
    char token[256];
    int token_len = 0;
    int in_quotes = 0;

    shell_cmd_t *cmd = &cmds[0];
    cmd->argc = 0;
    cmd->in_redir = REDIR_NONE;
    cmd->in_file[0] = 0;
    cmd->out_redir = REDIR_NONE;
    cmd->out_file[0] = 0;

    while (*cursor) {
        char c = *cursor;

        if (c == '"' || c == '\'') {
            if (!in_quotes) {
                in_quotes = c;
            } else if (in_quotes == c) {
                in_quotes = 0;
            }
            cursor++;
            continue;
        }

        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (token_len > 0) {
                token[token_len] = '\0';
                /* Expand variables in token before adding. */
                char expanded[512];
                shell_expand_variables(token, expanded, sizeof(expanded));
                if (cmd->argc < 32) {
                    cmd->argv[cmd->argc++] = kmalloc(shell_strlen(expanded) + 1);
                    for (int i = 0; i <= shell_strlen(expanded); i++)
                        cmd->argv[cmd->argc - 1][i] = expanded[i];
                }
                token_len = 0;
            }
            cursor++;
            continue;
        }

        if (!in_quotes && c == '|') {
            /* End of current command, start new one in pipeline. */
            if (token_len > 0) {
                token[token_len] = '\0';
                if (cmd->argc < 32) {
                    cmd->argv[cmd->argc++] = kmalloc(token_len + 1);
                    for (int i = 0; i <= token_len; i++)
                        cmd->argv[cmd->argc - 1][i] = token[i];
                }
                token_len = 0;
            }

            if (cmd->argc == 0) {
                console_error("Syntax error: empty command in pipeline");
                return -1;
            }

            cmd_count++;
            if (cmd_count >= MAX_PIPE_CMDS) {
                console_error("Too many commands in pipeline");
                return -1;
            }

            cmd = &cmds[cmd_count];
            cmd->argc = 0;
            cmd->in_redir = REDIR_NONE;
            cmd->in_file[0] = 0;
            cmd->out_redir = REDIR_NONE;
            cmd->out_file[0] = 0;

            cursor++;
            continue;
        }

        if (!in_quotes && c == '>') {
            cursor++;
            if (*cursor == '>') {
                /* >> append */
                cursor++;
                cmd->out_redir = REDIR_APPEND;
            } else {
                /* > */
                cmd->out_redir = REDIR_OUT;
            }

            /* Skip whitespace */
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;

            /* Parse output file name */
            int fn_len = 0;
            while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '|' && fn_len < FS_PATH_MAX - 1) {
                cmd->out_file[fn_len++] = *cursor++;
            }
            cmd->out_file[fn_len] = '\0';
            continue;
        }

        if (!in_quotes && c == '<') {
            cursor++;
            cmd->in_redir = REDIR_IN;

            /* Skip whitespace */
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;

            /* Parse input file name */
            int fn_len = 0;
            while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '|' && fn_len < FS_PATH_MAX - 1) {
                cmd->in_file[fn_len++] = *cursor++;
            }
            cmd->in_file[fn_len] = '\0';
            continue;
        }

        /* Regular character */
        if (token_len < 255) {
            token[token_len++] = c;
        }
        cursor++;
    }

    /* Handle last token */
    if (token_len > 0) {
        token[token_len] = '\0';
        if (cmd->argc < 32) {
            cmd->argv[cmd->argc++] = kmalloc(token_len + 1);
            for (int i = 0; i <= token_len; i++)
                cmd->argv[cmd->argc - 1][i] = token[i];
        }
    }

    if (cmd->argc == 0 && cmd_count == 0) {
        return -1;
    }

    return cmd_count + 1;
}

/* Expand shell variables in a string. */
static void shell_expand_variables(const char *input, char *output, uint32_t output_size)
{
    const char *in = input;
    char *out = output;
    uint32_t out_len = 0;

    while (*in && out_len < output_size - 1) {
        if (*in == '$') {
            in++;
            if (*in == '{') {
                in++;
                char var_name[MAX_VAR_NAME];
                uint32_t var_len = 0;
                while (*in && *in != '}' && var_len < MAX_VAR_NAME - 1) {
                    var_name[var_len++] = *in++;
                }
                var_name[var_len] = '\0';
                if (*in == '}')
                    in++;

                for (uint32_t i = 0; i < var_count; i++) {
                    if (shell_strcmp(shell_vars[i].name, var_name) == 0) {
                        const char *val = shell_vars[i].value;
                        while (*val && out_len < output_size - 1) {
                            *out++ = *val++;
                            out_len++;
                        }
                        break;
                    }
                }
            } else if (*in >= 'a' && *in <= 'z' || *in >= 'A' && *in <= 'Z' || *in == '_') {
                char var_name[MAX_VAR_NAME];
                uint32_t var_len = 0;
                while (*in && (*in >= 'a' && *in <= 'z' || *in >= 'A' && *in <= 'Z' || *in >= '0' && *in <= '9' || *in == '_') && var_len < MAX_VAR_NAME - 1) {
                    var_name[var_len++] = *in++;
                }
                var_name[var_len] = '\0';

                for (uint32_t i = 0; i < var_count; i++) {
                    if (shell_strcmp(shell_vars[i].name, var_name) == 0) {
                        const char *val = shell_vars[i].value;
                        while (*val && out_len < output_size - 1) {
                            *out++ = *val++;
                            out_len++;
                        }
                        break;
                    }
                }
            } else {
                *out++ = '$';
                out_len++;
            }
        } else {
            *out++ = *in++;
            out_len++;
        }
    }
    *out = '\0';
}

/* Get a variable value. */
const char *shell_get_var(const char *name)
{
    for (uint32_t i = 0; i < var_count; i++) {
        if (shell_strcmp(shell_vars[i].name, name) == 0) {
            return shell_vars[i].value;
        }
    }
    return 0;
}

/* Set a variable. */
int shell_set_var(const char *name, const char *value)
{
    if (shell_strlen(name) >= MAX_VAR_NAME || shell_strlen(value) >= MAX_VAR_VALUE)
        return -1;

    for (uint32_t i = 0; i < var_count; i++) {
        if (shell_strcmp(shell_vars[i].name, name) == 0) {
            for (uint32_t j = 0; j < shell_strlen(value); j++)
                shell_vars[i].value[j] = value[j];
            shell_vars[i].value[shell_strlen(value)] = '\0';
            return 0;
        }
    }

    if (var_count >= MAX_VARS)
        return -1;

    for (uint32_t i = 0; i < shell_strlen(name); i++)
        shell_vars[var_count].name[i] = name[i];
    shell_vars[var_count].name[shell_strlen(name)] = '\0';

    for (uint32_t i = 0; i < shell_strlen(value); i++)
        shell_vars[var_count].value[i] = value[i];
    shell_vars[var_count].value[shell_strlen(value)] = '\0';

    var_count++;
    return 0;
}

/* Unset a variable. */
int shell_unset_var(const char *name)
{
    for (uint32_t i = 0; i < var_count; i++) {
        if (shell_strcmp(shell_vars[i].name, name) == 0) {
            for (uint32_t j = i; j < var_count - 1; j++) {
                shell_vars[j] = shell_vars[j + 1];
            }
            var_count--;
            return 0;
        }
    }
    return -1;
}

static int shell_exec_cmd(shell_cmd_t *cmd, int in_fd, int out_fd)
{
    (void)in_fd; (void)out_fd;
    if (cmd->argc == 0)
        return -1;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return -1;

    int stdin_fd = 0, stdout_fd = 1;

    /* Set up input redirection */
    if (cmd->in_redir == REDIR_IN) {
        fs_handle_t fd;
        if (fs_open(cmd->in_file, FS_ROOT, FS_OPEN_READ, &fd) != FS_OK) {
            console_error("Cannot open input file: ");
            terminal_write(cmd->in_file);
            terminal_putchar('\n');
            return -1;
        }
        /* Find free fd slot */
        int fd_idx = -1;
        for (int i = 0; i < MAX_FDS; i++) {
            if (!((task_t *)task_get(scheduler_current_task()))->fd_table.open[i]) {
                fd_idx = i;
                break;
            }
        }
        if (fd_idx == -1) {
            fs_close(&fd);
            return -1;
        }
        task_t *task = (task_t *)task_get(scheduler_current_task());
        task->fd_table.handles[fd_idx] = fd;
        task->fd_table.open[fd_idx] = 1;
        stdin_fd = fd_idx;
    } else if (in_fd >= 0) {
        stdin_fd = in_fd;
    }

    /* Set up output redirection */
    if (cmd->out_redir == REDIR_OUT) {
        if (fs_write(cmd->out_file, FS_ROOT, "", 0) != FS_OK) {
            console_error("Cannot create output file: ");
            terminal_write(cmd->out_file);
            terminal_putchar('\n');
            return -1;
        }
        fs_handle_t fd;
        if (fs_open(cmd->out_file, FS_ROOT, 0, &fd) != FS_OK) {
            return -1;
        }
        int fd_idx = -1;
        for (int i = 0; i < MAX_FDS; i++) {
            if (!((task_t *)task_get(scheduler_current_task()))->fd_table.open[i]) {
                fd_idx = i;
                break;
            }
        }
        if (fd_idx == -1) {
            fs_close(&fd);
            return -1;
        }
        task_t *task = (task_t *)task_get(scheduler_current_task());
        task->fd_table.handles[fd_idx] = fd;
        task->fd_table.open[fd_idx] = 1;
        stdout_fd = fd_idx;
    } else if (cmd->out_redir == REDIR_APPEND) {
        fs_handle_t fd;
        if (fs_open(cmd->out_file, FS_ROOT, FS_OPEN_WRITE | FS_OPEN_APPEND, &fd) != FS_OK) {
            console_error("Cannot open output file for append: ");
            terminal_write(cmd->out_file);
            terminal_putchar('\n');
            return -1;
        }
        int fd_idx = -1;
        for (int i = 0; i < MAX_FDS; i++) {
            if (!((task_t *)task_get(scheduler_current_task()))->fd_table.open[i]) {
                fd_idx = i;
                break;
            }
        }
        if (fd_idx == -1) {
            fs_close(&fd);
            return -1;
        }
        task_t *task = (task_t *)task_get(scheduler_current_task());
        task->fd_table.handles[fd_idx] = fd;
        task->fd_table.open[fd_idx] = 1;
        stdout_fd = fd_idx;
    } else if (out_fd >= 0) {
        stdout_fd = out_fd;
    }

    /* Handle built-in commands (stdout may be pipe-captured). */
    if (cmd->argv[0][0] == 'e' && cmd->argv[0][1] == 'c' && cmd->argv[0][2] == 'h' && cmd->argv[0][3] == 'o' && cmd->argv[0][4] == '\0') {
        char ebuf[512]; uint32_t elen=0;
        for (int i = 1; i < cmd->argc && elen+1<sizeof(ebuf); i++) {
            const char *a=cmd->argv[i]; while(*a&&elen+1<sizeof(ebuf))ebuf[elen++]=*a++;
            if(i+1<cmd->argc&&elen+1<sizeof(ebuf))ebuf[elen++]=' ';
        }
        if(elen+1<sizeof(ebuf))ebuf[elen++]='\n';
        if (cmd->out_redir==REDIR_OUT||cmd->out_redir==REDIR_APPEND) {
            if(cmd->out_redir==REDIR_OUT){fs_write(cmd->out_file,FS_ROOT,ebuf,elen);}
            else{fs_handle_t h;if(fs_open(cmd->out_file,FS_ROOT,FS_OPEN_WRITE|FS_OPEN_APPEND,&h)==FS_OK){uint32_t w=0;fs_handle_write(&h,ebuf,elen,&w);fs_close(&h);}}
            sh_last_rc=0;return 0;
        }
        sh_emit(ebuf,elen); sh_last_rc=0; return 0;
    }
    if (cmd->argv[0][0]=='c'&&cmd->argv[0][1]=='a'&&cmd->argv[0][2]=='t'&&cmd->argv[0][3]=='\0') {
        /* cat: files, stdin-pipe, or < redirect; honors > redirect. */
        char cbuf[512]; uint32_t clen=0;
        if(cmd->argc<=1){
            if(sh_have_in){for(uint32_t i=0;i<sh_inlen&&clen<sizeof(cbuf);i++)cbuf[clen++]=sh_inbuf[i];}
            else if(cmd->in_redir==REDIR_IN){void *dt=0;uint32_t sz=0;
                if(fs_read(cmd->in_file,FS_ROOT,&dt,&sz)==FS_OK){char *q=(char*)dt;for(uint32_t i=0;i<sz&&clen<sizeof(cbuf);i++)cbuf[clen++]=q[i];kfree(dt);}}
        }else{
            for(int f=1;f<cmd->argc&&clen<sizeof(cbuf);f++){
                const char *pa=cmd->argv[f];
                if(pa[0]=='/'&&pa[1]=='d'&&pa[2]=='e'&&pa[3]=='v'&&pa[4]=='/'&&pa[5]=='n'&&pa[6]=='u'&&pa[7]=='l'&&pa[8]=='l'&&pa[9]=='\0'){/* /dev/null: nothing */}
                else if(pa[0]=='/'&&pa[1]=='d'&&pa[2]=='e'&&pa[3]=='v'&&pa[4]=='/'&&pa[5]=='z'&&pa[6]=='e'&&pa[7]=='r'&&pa[8]=='o'&&pa[9]=='\0'){
                    for(int z=0;z<64&&clen<sizeof(cbuf);z++)cbuf[clen++]='0';}
                else if(pa[0]=='/'&&pa[1]=='p'&&pa[2]=='r'&&pa[3]=='o'&&pa[4]=='c'&&pa[5]=='/'){
                    const char *sub=pa+6;
                    if(sub[0]=='v'&&sub[1]=='e'){const char *v="Lumen 0.3 i386\n";while(*v&&clen<sizeof(cbuf))cbuf[clen++]=*v++;}
                    else if(sub[0]=='m'){const char *m="meminfo: heap_used=";while(*m&&clen<sizeof(cbuf))cbuf[clen++]=*m++;
                        uint32_t u=heap_used();char nb[12];int nl=0;if(u==0)nb[nl++]='0';else{char r[12];int rn=0;while(u>0){r[rn++]='0'+u%10;u/=10;}while(rn>0)nb[nl++]=r[--rn];}
                        for(int k=0;k<nl&&clen<sizeof(cbuf);k++)cbuf[clen++]=nb[k];if(clen<sizeof(cbuf))cbuf[clen++]='\n';}
                    else if(sub[0]=='u'){const char *u="uptime ticks=";while(*u&&clen<sizeof(cbuf))cbuf[clen++]=*u++;
                        uint32_t t=timer_ticks;char nb[12];int nl=0;if(t==0)nb[nl++]='0';else{char r[12];int rn=0;while(t>0){r[rn++]='0'+t%10;t/=10;}while(rn>0)nb[nl++]=r[--rn];}
                        for(int k=0;k<nl&&clen<sizeof(cbuf);k++)cbuf[clen++]=nb[k];if(clen<sizeof(cbuf))cbuf[clen++]='\n';}
                }
                else{void *dt=0;uint32_t sz=0;
                    if(fs_read(pa,FS_ROOT,&dt,&sz)==FS_OK){char *q=(char*)dt;for(uint32_t i=0;i<sz&&clen<sizeof(cbuf);i++)cbuf[clen++]=q[i];kfree(dt);}}
            }
        }
        if(cmd->out_redir==REDIR_OUT){fs_write(cmd->out_file,FS_ROOT,cbuf,clen);}
        else if(cmd->out_redir==REDIR_APPEND){fs_handle_t h;if(fs_open(cmd->out_file,FS_ROOT,FS_OPEN_WRITE|FS_OPEN_APPEND,&h)==FS_OK){uint32_t w=0;fs_handle_write(&h,cbuf,clen,&w);fs_close(&h);}}
        else sh_emit(cbuf,clen);
        sh_last_rc=0; return 0;
    }
    if (cmd->argv[0][0]=='w'&&cmd->argv[0][1]=='c'&&cmd->argv[0][2]=='\0') {
        uint32_t n = sh_have_in ? sh_inlen : 0;
        char nb[12]; int nl=0;
        if(n==0)nb[nl++]='0';else{char r[12];int rn=0;uint32_t t=n;while(t>0){r[rn++]='0'+t%10;t/=10;}while(rn>0)nb[nl++]=r[--rn];}
        nb[nl++]='\n'; sh_emit(nb,nl); sh_last_rc=0; return 0;
    }
    if (cmd->argv[0][0]=='s'&&cmd->argv[0][1]=='l'&&cmd->argv[0][2]=='e'&&cmd->argv[0][3]=='e'&&cmd->argv[0][4]=='p'&&cmd->argv[0][5]=='\0') {
        uint32_t n=1;if(cmd->argc>1){n=0;const char *p=cmd->argv[1];while(*p>='0'&&*p<='9'){n=n*10+(uint32_t)(*p-'0');p++;}}
        uint32_t end=timer_ticks+n*100;while(timer_ticks<end)__asm__ volatile("hlt");
        sh_last_rc=0;return 0;
    }

    if (cmd->argv[0][0] == 'c' && cmd->argv[0][1] == 'd' && cmd->argv[0][2] == '\0') {
        if (cmd->argc < 2) {
            console_error("cd: missing argument");
            return -1;
        }
        uint32_t dir_ino = 0;
        if (fs_resolve_dir(cmd->argv[1], FS_ROOT, &dir_ino) != FS_OK) { console_error("cd: no such directory"); return -1; }
        task_t *self = (task_t *)task_get(scheduler_current_task());
        if (self) self->cwd_inode = dir_ino;
        return 0;
    }

    /* External command - use loader_spawn. */
    int task_id = loader_spawn(cmd->argv[0]);
    if (task_id >= 0 && sh_bg_next) {
        int jid = sh_job_add(task_id, cmd->argv[0], 1);
        terminal_write("[bg job "); terminal_write_u32((uint32_t)jid);
        terminal_write(" pid "); terminal_write_u32((uint32_t)task_id);
        terminal_write("]\n");
    }
    sh_bg_next = 0;
    if (task_id < 0) {
        console_error("Command not found: ");
        terminal_write(cmd->argv[0]);
        terminal_putchar('\n');
        return -1;
    }

    return 0;
}

/* Execute a pipeline of commands. */
static int shell_exec_pipeline(shell_cmd_t *cmds, int cmd_count)
{
    if (cmd_count == 1) {
        sh_cap=0; sh_have_in=0;
        return shell_exec_cmd(&cmds[0], 0, 1);
    }

    /* Real in-memory pipes: stage i's captured stdout feeds stage i+1's stdin. */
    sh_have_in=0; sh_inlen=0;
    for (int i = 0; i < cmd_count; i++) {
        sh_cap = (i < cmd_count - 1) ? 1 : 0;
        sh_plen = 0;
        int ret = shell_exec_cmd(&cmds[i], 0, 1);
        if (ret < 0) { sh_cap=0; sh_have_in=0; return -1; }
        if (i < cmd_count - 1) {
            for (uint32_t k=0;k<sh_plen&&k<SH_PIPEBUF;k++) sh_inbuf[k]=sh_pbuf[k];
            sh_inlen=sh_plen; sh_have_in=1;
        }
    }
    sh_cap=0; sh_have_in=0;
    return 0;
}



extern volatile uint32_t timer_ticks;

/*
 * Ring 3 VM protection test.
 *
 * 0x00001000 belongs to the kernel's supervisor-only
 * identity mapping. Ring 3 must NOT be able to read it.
 *
 * mov eax, [0x1000]
 * jmp $
 */
static const uint8_t vm_fault_program[] = {
    0xA1, 0x00, 0x10, 0x00, 0x00,
    0xEB, 0xFE
};

/*
 * Ring 3 privilege protection test.
 *
 * CLI is a privileged instruction and must raise #GP
 * when executed at CPL 3.
 *
 * cli
 * jmp $
 */
static const uint8_t privilege_fault_program[] = {
    0xFA,
    0xEB, 0xFE
};


typedef struct { const char *name; const char *args; const char *desc; } shell_cmd_info_t;

/* One row per command the shell accepts. `help` prints this table,
 * so adding a command means adding a row here. */
static const shell_cmd_info_t shell_cmd_table[] = {
    { "help", "", "list all commands" },
    { "clear", "", "clear the screen" },
    { "echo", "TEXT", "print text (| > >> aware)" },
    { "cat", "[FILE]", "print file(s), stdin, /proc/*, /dev/*" },
    { "wc", "", "count piped bytes" },
    { "sleep", "SECS", "wait (100 Hz ticks)" },
    { "cd", "DIR", "change directory" },
    { "about", "", "about Lumen" },
    { "version", "", "kernel version" },
    { "mem", "", "memory usage" },
    { "uptime", "", "ticks since boot" },
    { "uname", "", "system info" },
    { "top", "", "task table snapshot" },
    { "dmesg", "", "kernel log" },
    { "task", "", "spawn demo task" },
    { "taskuser", "", "spawn Ring 3 demo task" },
    { "taskkill", "PID", "terminate a task" },
    { "kill", "PID", "send SIGKILL" },
    { "tasks", "", "list tasks" },
    { "ps", "", "list tasks (alias)" },
    { "jobs", "", "list background jobs" },
    { "fg", "JOBID", "wait for a background job" },
    { "bg", "JOBID", "resume a background job" },
    { "wait", "PID", "wait for child task" },
    { "vmtest", "", "Ring 3 memory-protection test" },
    { "privtest", "", "Ring 3 privilege test" },
    { "exittest", "", "Ring 3 exit test" },
    { "usermode", "", "enter legacy Ring 3 demo" },
    { "install", "FILE", "load flat binary as task" },
    { "run", "FILE|SCRIPT", "run ELF program or shell script" },
    { "if", "exists F then CMD", "run CMD if file exists" },
    { "edit", "FILE", "line editor (. saves)" },
    { "format", "", "format disk as LumenFS v2" },
    { "ls", "[PATH]", "list directory (also /proc /dev)" },
    { "write", "FILE TEXT", "write file" },
    { "rm", "FILE", "remove file" },
    { "mkdir", "DIR", "make directory" },
    { "rmdir", "DIR", "remove directory" },
    { "ln", "A B", "hard link" },
    { "mv", "A B", "rename" },
    { "readlink", "LINK", "read symlink target" },
    { "chmod", "MODE FILE", "change mode" },
    { "chown", "UID[:GID] FILE", "change owner" },
    { "df", "", "disk free" },
    { "diskinfo", "", "ATA drive info" },
    { "storage-test", "", "filesystem self-check" },
    { "date", "", "RTC date/time" },
    { "mount", "", "mount status" },
    { "settings", "", "list settings" },
    { "set", "KEY VAL", "set + persist setting" },
    { "get", "KEY", "print setting" },
    { "hostname", "[NAME]", "show/set hostname" },
    { "users", "", "list users" },
    { "useradd", "USER PASS", "add user" },
    { "login", "USER PASS", "log in" },
    { "whoami", "", "current user" },
    { "sudo", "CMD", "run command, log it" },
    { "ifconfig", "", "network interfaces" },
    { "ping", "[IP]", "ICMP echo (default 127.0.0.1)" },
    { "netstat", "", "network + NIC status" },
    { "arp", "", "ARP table" },
    { "pkg", "li|in|rm", "packages" },
    { "gui", "", "text-mode desktop demo" },
    { "guigfx", "", "Mode 13h graphics demo" },
    { "files", "", "two-window desktop demo" },
    { "poweroff", "", "halt" },
    { "reboot", "", "reboot" },
    { "cron", "[TICKS CMD]", "list/add/remove timer jobs" },
};

static void shell_print_help(void)
{
    for (uint32_t i = 0;
         i < sizeof(shell_cmd_table) / sizeof(shell_cmd_table[0]);
         i++) {
        terminal_write(shell_cmd_table[i].name);
        if (shell_cmd_table[i].args[0]) {
            terminal_write(" ");
            terminal_write(shell_cmd_table[i].args);
        }
        terminal_write(" - ");
        terminal_write(shell_cmd_table[i].desc);
        terminal_putchar('\n');
    }
}


static void shell_prompt(void)
{
    terminal_write(LUMEN_PROMPT);
}

/*
 * Public so the line editor can redraw the prompt after a Ctrl-L
 * clear or a Ctrl-C abandon, when it owns the screen and cannot let
 * shell_handle_line() print one for it.
 */
void shell_print_prompt(void)
{
    shell_prompt();
}

uint32_t shell_strlen(const char *s)
{
    uint32_t n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/* Render a directory, one entry per line, with a trailing slash on
 * directories so the listing is unambiguous. */
static void shell_list(const char *path)
{
    if (!fs_is_mounted()) {
        console_error("No filesystem mounted (run 'format')");
        return;
    }

    uint32_t count = 0;
    int rc = fs_count_entries(path, FS_ROOT, &count);

    if (rc != FS_OK) {
        console_error("ls: ");
        terminal_write(fs_strerror(rc));
        terminal_putchar('\n');
        return;
    }

    if (count == 0) {
        console_info("(empty)");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ino = 0;
        const char *name = 0;
        uint8_t type = 0;

        if (fs_list(path, FS_ROOT, i, &ino, &name, &type) != FS_OK)
            break;

        terminal_write(name);

        if (type == FS_TYPE_DIR) {
            terminal_putchar('/');
        } else {
            /* Show the size, which is the thing that actually
             * distinguishes two files. */
            terminal_write("  ");
            fs_inode_t meta;

            if (fs_stat_by_inode(ino, &meta) == FS_OK)
                terminal_write_u32(meta.size);

            terminal_write(" B");
        }

        terminal_putchar('\n');
    }
}

static uint32_t shell_parse_uint(const char *text)
{
    uint32_t value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (uint32_t)(*text - '0');
        text++;
    }

    return value;
}

static void shell_print_uint(uint32_t value)
{
    char digits[10];
    int count = 0;

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0) {
        digits[count++] = '0' + (value % 10);
        value /= 10;
    }

    while (count > 0)
        terminal_putchar(digits[--count]);
}

static int string_starts_with(const char *text, const char *prefix)
{
    int i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i])
            return 0;

        i++;
    }

    return 1;
}

void shell_init(void)
{
    shell_prompt();
}

void shell_handle_line(const char *line)
{
    cron_poll();
    /* line-editor mode: accumulate until lone '.' */
    if (sh_ed_on) {
        if (line[0]=='.'&&line[1]=='\0') {
            fs_write(sh_ed_file, FS_ROOT, sh_ed_buf?sh_ed_buf:"", sh_ed_len);
            if (sh_ed_buf){kfree(sh_ed_buf);sh_ed_buf=0;}
            sh_ed_len=0; sh_ed_cap=0; sh_ed_on=0;
            console_info("saved");
            shell_prompt(); return;
        }
        uint32_t ll=0; while(line[ll])ll++;
        if (sh_ed_len+ll+1 >= sh_ed_cap) {
            uint32_t nc = sh_ed_cap?sh_ed_cap*2:512; if(nc<sh_ed_len+ll+2)nc=sh_ed_len+ll+2; if(nc>4096)nc=4096;
            char *nb=(char*)kmalloc(nc);
            if(!nb){console_error("editor out of memory");shell_prompt();return;}
            for(uint32_t i=0;i<sh_ed_len;i++)nb[i]=sh_ed_buf[i];
            if(sh_ed_buf)kfree(sh_ed_buf);
            sh_ed_buf=nb; sh_ed_cap=nc;
        }
        if (sh_ed_len+ll+1 < sh_ed_cap) {
            for(uint32_t i=0;i<ll;i++)sh_ed_buf[sh_ed_len++]=line[i];
            sh_ed_buf[sh_ed_len++]='\n';
        }
        return;
    }
    /* ';' command separators (quote-aware). */
    {
        int q=0;
        for (int i=0; line[i]; i++) {
            if(line[i]=='"'||line[i]=='\'')q=q?(q==line[i]?0:q):line[i];
            if(!q&&line[i]==';'){
                char a[256],b[256];int n=0;
                for(int k=0;k<i&&k<255;k++){a[k]=line[k];n=k+1;}a[n]=0;
                int m=0;for(int k=i+1;line[k]&&m<255;k++)b[m++]=line[k];b[m]=0;
                shell_handle_line(a);shell_handle_line(b);return;
            }
        }
    }
    /* trailing '&' -> background job */
    {
        int e=0;while(line[e])e++;int t=e;
        while(t>0&&(line[t-1]==' '||line[t-1]=='\t'))t--;
        if(t>0&&line[t-1]=='&'){
            char nb[256];int n=t-1;if(n>255)n=255;
            for(int k=0;k<n;k++)nb[k]=line[k];nb[n]=0;
            sh_bg_next=1;shell_handle_line(nb);sh_bg_next=0;
            shell_prompt();return;
        }
    }
    if (line[0] == '\0') {
        shell_prompt();
        return;
    }

    /* Check for pipes or redirection operators first. */
    int has_pipe = 0;
    int has_redir = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '|') has_pipe = 1;
        if (*p == '>' || *p == '<') has_redir = 1;
    }

    if (has_pipe || has_redir) {
        shell_cmd_t cmds[MAX_PIPE_CMDS];
        for (int i = 0; i < MAX_PIPE_CMDS; i++) {
            cmds[i].argc = 0;
            cmds[i].in_redir = REDIR_NONE;
            cmds[i].in_file[0] = '\0';
            cmds[i].out_redir = REDIR_NONE;
            cmds[i].out_file[0] = '\0';
        }

        int cmd_count = shell_parse_pipeline(line, cmds, MAX_PIPE_CMDS);
        if (cmd_count > 0) {
            shell_exec_pipeline(cmds, cmd_count);
            /* Free allocated argument strings */
            for (int i = 0; i < cmd_count; i++) {
                for (int j = 0; j < cmds[i].argc; j++)
                    kfree(cmds[i].argv[j]);
            }
            shell_prompt();
            return;
        }

    if (line[0] == 'h' &&
        line[1] == 'e' &&
        line[2] == 'l' &&
        line[3] == 'p' &&
        line[4] == '\0') {

        shell_print_help();
    }
    else if (string_starts_with(line, "echo ")) {
        terminal_write(line + 5);
        terminal_putchar('\n');
    }
    else if (line[0] == 'c' &&
             line[1] == 'l' &&
             line[2] == 'e' &&
             line[3] == 'a' &&
             line[4] == 'r' &&
             line[5] == '\0') {
        terminal_clear();
    } else if (line[0] == 'g' &&
               line[1] == 'u' &&
               line[2] == 'i' &&
               line[3] == '\0') {
        gui_demo();
    } else if (line[0]=='s'&&line[1]=='e'&&line[2]=='t'&&line[3]=='t'&&line[4]=='i'&&line[5]=='n'&&line[6]=='g'&&line[7]=='s'&&(line[8]=='\0'||line[8]==' ')) {
        settings_list();
    } else if (line[0]=='s'&&line[1]=='e'&&line[2]=='t'&&(line[3]=='\0'||line[3]==' ')) {
        const char *a=line+3; while(*a==' ')a++;
        const char *sp=a; while(*sp&&*sp!=' ')sp++;
        if(!*a||!*sp){console_error("usage: set KEY VALUE");}
        else{char k[24],v[48];int i=0;while(*a&&*a!=' '&&i<23){k[i++]=*a++;}k[i]=0;while(*a==' ')a++;i=0;while(*a&&i<47){v[i++]=*a++;}v[i]=0;settings_set(k,v);settings_save();console_info("setting saved");}
    } else if (line[0]=='g'&&line[1]=='e'&&line[2]=='t'&&(line[3]=='\0'||line[3]==' ')) {
        const char *a=line+3; while(*a==' ')a++;
        char tmp[48]; if(settings_get(a,tmp,sizeof(tmp))==0){terminal_write(tmp);terminal_putchar('\n');}else console_error("unknown key (try 'settings')");
    } else if (line[0]=='h'&&line[1]=='o'&&line[2]=='s'&&line[3]=='t'&&line[4]=='n'&&line[5]=='a'&&line[6]=='m'&&line[7]=='e'&&(line[8]=='\0'||line[8]==' ')) {
        const char *a=line+8; while(*a==' ')a++;
        if(!*a){char tmp[48];settings_get("hostname",tmp,sizeof(tmp));terminal_write(tmp);terminal_putchar('\n');}
        else{settings_set("hostname",a);settings_save();console_info("hostname updated");}
    } else if (line[0]=='p'&&line[1]=='o'&&line[2]=='w'&&line[3]=='e'&&line[4]=='r'&&line[5]=='o'&&line[6]=='f'&&line[7]=='f'&&line[8]=='\0') {
        console_info("powering off..."); __asm__ volatile("cli; hlt");
    } else if (line[0]=='r'&&line[1]=='e'&&line[2]=='b'&&line[3]=='o'&&line[4]=='o'&&line[5]=='t'&&line[6]=='\0') {
        console_info("rebooting..."); __asm__ volatile("cli; hlt");
    } else if (line[0]=='u'&&line[1]=='s'&&line[2]=='e'&&line[3]=='r'&&line[4]=='s'&&line[5]=='\0') {
        users_list();
    } else if (line[0]=='u'&&line[1]=='s'&&line[2]=='e'&&line[3]=='r'&&line[4]=='a'&&line[5]=='d'&&line[6]=='d'&&(line[7]=='\0'||line[7]==' ')) {
        const char *a=line+7;while(*a==' ')a++;
        char un[24];int i=0;while(*a&&*a!=' '&&i<23){un[i++]=*a++;}un[i]=0;
        while(*a==' ')a++;char pw[24];i=0;while(*a&&i<23){pw[i++]=*a++;}pw[i]=0;
        if(*un&&*pw&&user_add(un,pw,1000)==0)console_info("user added");else console_error("useradd failed");
    } else if (line[0]=='w'&&line[1]=='h'&&line[2]=='o'&&line[3]=='a'&&line[4]=='m'&&line[5]=='i'&&line[6]=='\0') {
        terminal_write(user_current());terminal_putchar('\n');
    } else if (line[0]=='l'&&line[1]=='o'&&line[2]=='g'&&line[3]=='i'&&line[4]=='n'&&(line[5]=='\0'||line[5]==' ')) {
        const char *a=line+5;while(*a==' ')a++;
        char un[24];int i=0;while(*a&&*a!=' '&&i<23){un[i++]=*a++;}un[i]=0;
        while(*a==' ')a++;char pw[24];i=0;while(*a&&i<23){pw[i++]=*a++;}pw[i]=0;
        if(user_login(un,pw)==0)console_info("login ok");else console_error("login failed");
    } else if (line[0]=='i'&&line[1]=='f'&&line[2]=='c'&&line[3]=='o'&&line[4]=='n'&&line[5]=='f'&&line[6]=='i'&&line[7]=='g'&&line[8]=='\0') {
        net_ifconfig();net_stat();
    } else if (line[0]=='p'&&line[1]=='i'&&line[2]=='n'&&line[3]=='g'&&(line[4]=='\0'||line[4]==' ')) {
        const char *a=line+4;while(*a==' ')a++;
        uint32_t ip=0x7F000001u;
        if(*a){uint32_t b[4]={0,0,0,0};
            for(int o=0;o<4;o++){while(*a>='0'&&*a<='9'){b[o]=b[o]*10+(uint32_t)(*a-'0');a++;}if(*a=='.')a++;}
            ip=(b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3];}
        int r=net_ping(ip);
        if(r==0)console_info("pong: reply received");
        else if(r==-2)console_error("ping: ARP failed (no route/no NIC?)");
        else if(r==-3)console_error("ping: sent, no reply (QEMU user-net drops ICMP)");
        else console_error("ping failed");
    } else if(line[0]=='n'&&line[1]=='e'&&line[2]=='t'&&line[3]=='s'&&line[4]=='t'&&line[5]=='a'&&line[6]=='t'&&line[7]=='\0'){
        net_ifconfig();net_stat();
    } else if(line[0]=='a'&&line[1]=='r'&&line[2]=='p'&&line[3]=='\0'){
        nic_dump();
    } else if (line[0]=='p'&&line[1]=='k'&&line[2]=='g'&&(line[3]=='\0'||line[3]==' ')) {
        const char *a=line+3;while(*a==' ')a++;
        if(a[0]=='l'&&a[1]=='i'){pkg_list();}
        else console_info("usage: pkg li[st] | pkg in <name> | pkg rm <name>");
        if(a[0]=='i'&&a[1]=='n'){const char *n=a+2;while(*n==' ')n++;if(*n)pkg_install(n,"1.0");}
        if(a[0]=='r'&&a[1]=='m'){const char *n=a+2;while(*n==' ')n++;if(*n)pkg_remove(n);}
    } else if (line[0]=='f'&&line[1]=='i'&&line[2]=='l'&&line[3]=='e'&&line[4]=='s'&&line[5]=='\0') {
        gui_windows_demo();
    } else if(line[0]=='j'&&line[1]=='o'&&line[2]=='b'&&line[3]=='s'&&line[4]=='\0'){
        for(int i=0;i<8;i++)if(sh_jobs[i].used){
            terminal_write_u32((uint32_t)i);terminal_write(": pid ");
            terminal_write_u32((uint32_t)sh_jobs[i].pid);terminal_write(" ");
            terminal_write(sh_jobs[i].cmd);
            const task_t *t=task_get((uint32_t)sh_jobs[i].pid);
            terminal_write(t?(t->state==TASK_TERMINATED?" done\n":" running\n"):" gone\n");
        }
    } else if(line[0]=='f'&&line[1]=='g'&&(line[2]=='\0'||line[2]==' ')){
        int id=-1;const char *a=line+2;while(*a==' ')a++;while(*a>='0'&&*a<='9'){if(id<0)id=0;id=id*10+(*a-'0');a++;}
        if(id<0||id>=8||!sh_jobs[id].used)console_error("usage: fg JOBID");
        else{task_wait((uint32_t)sh_jobs[id].pid);sh_jobs[id].used=0;console_info("job done");}
    } else if(line[0]=='b'&&line[1]=='g'&&(line[2]=='\0'||line[2]==' ')){
        int id=-1;const char *a=line+2;while(*a==' ')a++;while(*a>='0'&&*a<='9'){if(id<0)id=0;id=id*10+(*a-'0');a++;}
        if(id<0||id>=8||!sh_jobs[id].used)console_error("usage: bg JOBID");
        else{task_wake((uint32_t)sh_jobs[id].pid);console_info("job resumed");}
    } else if(string_starts_with(line,"if exists ")){
        const char *rest=line+10;const char *th=0;
        for(const char *q=rest;*q;q++)if(q[0]==' '&&q[1]=='t'&&q[2]=='h'&&q[3]=='e'&&q[4]=='n'&&(q[5]==' '||q[5]=='\0')){th=q;break;}
        if(!th)console_error("usage: if exists FILE then CMD");
        else{char fp[128];int n=0;for(const char *q=rest;q<th&&n<127;q++)fp[n++]=*q;while(n>0&&fp[n-1]==' ')n--;fp[n]=0;
            fs_inode_t meta;
            if(fs_stat(fp,FS_ROOT,&meta)==FS_OK){const char *cc=th+5;while(*cc==' ')cc++;shell_handle_line(cc);return;}}
    } else if(string_starts_with(line,"run ")){
        const char *fp=line+4;while(*fp==' ')fp++;
        void *dt=0;uint32_t sz=0;
        if(fs_read(fp,FS_ROOT,&dt,&sz)!=FS_OK){console_error("run: cannot read file");}
        else{char *q=(char*)dt;uint32_t s=0;
            while(s<sz){char ln[256];int n=0;
                while(s<sz&&q[s]!='\n'&&n<255)ln[n++]=q[s++];if(s<sz&&q[s]=='\n')s++;
                ln[n]=0;const char *t=ln;while(*t==' '||*t=='\t')t++;
                if(*t&&*t!='#')shell_handle_line(t);
            }kfree(dt);}
    } else if(string_starts_with(line,"edit ")){
        const char *fp=line+5;while(*fp==' ')fp++;
        if(!*fp){console_error("usage: edit FILE");}
        else{
            int n=0;while(fp[n]&&n<127){sh_ed_file[n]=fp[n];n++;}sh_ed_file[n]=0;
            void *dt=0;uint32_t sz=0;
            sh_ed_len=0;sh_ed_cap=0;sh_ed_buf=0;
            if(fs_read(fp,FS_ROOT,&dt,&sz)==FS_OK&&sz<4096){
                sh_ed_buf=(char*)kmalloc(sz+512);char *q=(char*)dt;
                for(uint32_t i=0;i<sz;i++)sh_ed_buf[i]=q[i];
                sh_ed_len=sz;sh_ed_cap=sz+512;kfree(dt);
            }
            sh_ed_on=1;
            console_info("-- editing: type lines, '.' alone to save --");
            return;
        }
    } else if (line[0]=='u'&&line[1]=='n'&&line[2]=='a'&&line[3]=='m'&&line[4]=='e'&&line[5]=='\0') {
        proc_uname();
    } else if (line[0]=='t'&&line[1]=='o'&&line[2]=='p'&&line[3]=='\0') {
        proc_top();
    } else if (line[0]=='d'&&line[1]=='m'&&line[2]=='e'&&line[3]=='s'&&line[4]=='g'&&line[5]=='\0') {
        klog_dump();
    } else if (line[0]=='k'&&line[1]=='i'&&line[2]=='l'&&line[3]=='l'&&(line[4]=='\0'||line[4]==' ')) {
        const char *a=line+4;while(*a==' ')a++;
        uint32_t pid=0;while(*a>='0'&&*a<='9'){pid=pid*10+(uint32_t)(*a-'0');a++;}
        if(sys_kill(pid,9))console_info("killed");else console_error("kill failed");
    } else if (line[0]=='c'&&line[1]=='h'&&line[2]=='m'&&line[3]=='o'&&line[4]=='d'&&(line[5]=='\0'||line[5]==' ')) {
        const char *a=line+5;while(*a==' ')a++;
        uint32_t mode=0;while(*a>='0'&&*a<='7'){mode=mode*8+(uint32_t)(*a-'0');a++;}
        while(*a==' ')a++;
        if(!*a)console_error("usage: chmod MODE FILE");
        else if(fs_chmod(a,FS_ROOT,(uint8_t)mode)==FS_OK)console_info("chmod ok");else console_error("chmod failed");
    } else if (line[0]=='s'&&line[1]=='u'&&line[2]=='d'&&line[3]=='o'&&(line[4]=='\0'||line[4]==' ')) {
        const char *a=line+4;while(*a==' ')a++;
        if(!*a)console_error("usage: sudo CMD");
        else{shell_handle_line(a);klog_put("sudo exec");}
    }
    else if (line[0]=='c'&&line[1]=='h'&&line[2]=='o'&&line[3]=='w'&&line[4]=='n'&&(line[5]=='\0'||line[5]==' ')) {
        const char *a=line+5;while(*a==' ')a++;
        uint32_t uid=0;while(*a>='0'&&*a<='9'){uid=uid*10+(uint32_t)(*a-'0');a++;}
        uint32_t gid=uid;
        if(*a==':'){a++;gid=0;while(*a>='0'&&*a<='9'){gid=gid*10+(uint32_t)(*a-'0');a++;}}
        while(*a==' ')a++;
        if(!*a)console_error("usage: chown UID[:GID] FILE");
        else if(fs_chown(a,FS_ROOT,(uint16_t)uid,(uint16_t)gid)==FS_OK)console_info("chown ok");else console_error("chown failed");
    } else if (line[0]=='m'&&line[1]=='o'&&line[2]=='u'&&line[3]=='n'&&line[4]=='t'&&(line[5]=='\0'||line[5]==' ')) {
        if(fs_is_mounted()){console_info("LumenFS mounted");}
        else if(fs_mount()==FS_OK||fs_is_mounted())console_info("mounted ok");
        else console_error("mount failed (run format?)");
    } else if (line[0]=='c'&&line[1]=='r'&&line[2]=='o'&&line[3]=='n'&&(line[4]=='\0'||line[4]==' ')) {
        const char *a=line+4;while(*a==' ')a++;
        if(!*a){cron_list();}
        else if(a[0]=='r'&&a[1]=='m'){int id=(a[2]>='0'&&a[2]<='9')?(a[2]-'0'):-1;if(cron_remove(id)==0)console_info("cron removed");else console_error("cron rm failed");}
        else{/* cron <ticks> <cmd> */
            uint32_t t=0;while(*a>='0'&&*a<='9'){t=t*10+(uint32_t)(*a-'0');a++;}
            while(*a==' ')a++;
            if(t==0||!*a)console_error("usage: cron TICKS CMD | cron | cron rm ID");
            else{int id=cron_add(a,t);if(id>=0){terminal_write("cron job ");terminal_write_u32((uint32_t)id);terminal_putchar('\n');}else console_error("cron full");}}
    } else if (line[0]=='g'&&line[1]=='u'&&line[2]=='i'&&line[3]=='g'&&line[4]=='f'&&line[5]=='x'&&line[6]=='\0') {
        gfx_demo();
    } else if (line[0] == 'a' &&
             line[1] == 'b' &&
             line[2] == 'o' &&
             line[3] == 'u' &&
             line[4] == 't' &&
             line[5] == '\0') {
        console_info(LUMEN_NAME " OS.");
        console_info("Built from scratch in C and x86 assembly.");
        console_info("Custom bootloader, kernel, interrupts, memory, keyboard, and shell.");
    }
    else if (line[0] == 'v' &&
             line[1] == 'e' &&
             line[2] == 'r' &&
             line[3] == 's' &&
             line[4] == 'i' &&
             line[5] == 'o' &&
             line[6] == 'n' &&
             line[7] == '\0') {
        console_info(LUMEN_NAME " OS version " LUMEN_VERSION);
    }
    else if (line[0] == 'm' &&
             line[1] == 'e' &&
             line[2] == 'm' &&
             line[3] == '\0') {
        terminal_write("[INFO] Heap used: ");
        shell_print_uint(heap_used());
        terminal_write(" bytes");
        terminal_putchar('\n');
    }
    else if (line[0] == 'u' &&
             line[1] == 'p' &&
             line[2] == 't' &&
             line[3] == 'i' &&
             line[4] == 'm' &&
             line[5] == 'e' &&
             line[6] == '\0') {
        terminal_write("[INFO] Uptime: ");
        shell_print_uint(timer_ticks / 100);
        terminal_write(" seconds");
        terminal_putchar('\n');
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == '\0') {
        int id = task_create();

        if (id >= 0) {
            terminal_write("[INFO] Task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("Task creation failed");
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 'u' &&
             line[5] == 's' &&
             line[6] == 'e' &&
             line[7] == 'r' &&
             line[8] == '\0') {
        int id = task_create_with_privilege(USER_RING);

        if (id >= 0) {
            terminal_write("[INFO] User task created: ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("User task creation failed");
        }
    }
    else if (line[0] == 'v' &&
             line[1] == 'm' &&
             line[2] == 't' &&
             line[3] == 'e' &&
             line[4] == 's' &&
             line[5] == 't' &&
             line[6] == '\0') {

        int id =
            task_create_user_program(
                vm_fault_program,
                sizeof(vm_fault_program)
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] VM protection test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');

            console_info(
                "Expected: Ring 3 page fault"
            );
        } else {
            console_error(
                "VM protection test creation failed"
            );
        }
    }
    else if (line[0] == 'p' &&
             line[1] == 'r' &&
             line[2] == 'i' &&
             line[3] == 'v' &&
             line[4] == 't' &&
             line[5] == 'e' &&
             line[6] == 's' &&
             line[7] == 't' &&
             line[8] == '\0') {

        int id =
            task_create_user_program(
                privilege_fault_program,
                sizeof(privilege_fault_program)
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] Privilege protection test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');

            console_info(
                "Expected: Ring 3 general protection fault"
            );
        } else {
            console_error(
                "Privilege protection test creation failed"
            );
        }
    }
    else if (line[0] == 'e' &&
             line[1] == 'x' &&
             line[2] == 'i' &&
             line[3] == 't' &&
             line[4] == 't' &&
             line[5] == 'e' &&
             line[6] == 's' &&
             line[7] == 't' &&
             line[8] == '\0') {

        int id =
            task_create_user_program(
                ring3_exit_program,
                ring3_exit_program_size
            );

        if (id >= 0) {
            terminal_write(
                "[INFO] Exit test task: ID "
            );
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        } else {
            console_error("Exit test task creation failed");
        }
    }
    else if (line[0] == 'u' &&
             line[1] == 's' &&
             line[2] == 'e' &&
             line[3] == 'r' &&
             line[4] == 'm' &&
             line[5] == 'o' &&
             line[6] == 'd' &&
             line[7] == 'e' &&
             line[8] == '\0') {
        console_info("Entering user mode...");
        if (!ring3_init()) {
            console_error("Ring 3 initialization: FAIL");
        } else {
            console_info("Entering Ring 3...");
            __asm__ volatile ("call enter_user_mode");
        }
    }
    else if (string_starts_with(line, "taskkill ")) {
        uint32_t id = shell_parse_uint(line + 9);

        if (task_terminate(id) == 0) {
            terminal_write("[INFO] Task terminated: ID ");
            shell_print_uint(id);
            terminal_putchar('\n');
        } else {
            console_error("Task termination failed");
        }
    }
    else if (line[0] == 'p' &&
             line[1] == 's' &&
             line[2] == '\0') {

        terminal_write("PID PPID STATE RING\n");

        for (uint32_t id = 0; id <= MAX_TASKS; id++) {
            const task_t *task = task_get(id);

            if (task == 0)
                continue;

            terminal_write(" ");
            shell_print_uint(task->id);
            terminal_write("   ");
            shell_print_uint(task->parent_id);
            terminal_write("   ");

            if (task->state == TASK_RUNNING) {
                terminal_write("RUNNING");
            } else if (task->state == TASK_READY) {
                terminal_write("READY");
            } else if (task->state == TASK_BLOCKED) {
                terminal_write("BLOCKED");
            } else if (task->state == TASK_TERMINATED) {
                terminal_write("TERMINATED");
            } else {
                terminal_write("UNUSED");
            }

            terminal_write(" ");

            if (task->privilege == KERNEL_RING) {
                terminal_write("0");
            } else {
                terminal_write("3");
            }

            terminal_putchar('\n');
        }
    }
    else if (line[0] == 't' &&
             line[1] == 'a' &&
             line[2] == 's' &&
             line[3] == 'k' &&
             line[4] == 's' &&
             line[5] == '\0') {
        terminal_write("[INFO] Active tasks: ");
        shell_print_uint(task_count());
        terminal_putchar('\n');

        for (uint32_t id = 1; id < 100; id++) {
            const task_t *task = task_get(id);

            if (task != 0 &&
                task->state != TASK_UNUSED &&
                task->state != TASK_TERMINATED) {
                terminal_write("ID ");
                shell_print_uint(task->id);
                terminal_write(" READY");

                if (task->privilege == KERNEL_RING) {
                    terminal_write(" RING0");
                } else if (task->privilege == USER_RING) {
                    terminal_write(" RING3");
                }

                terminal_putchar('\n');
            }
        }
    }
    else if (string_starts_with(line, "wait ")) {
        uint32_t id = shell_parse_uint(line + 5);

        int result = task_wait(id);

        if (result >= 0) {
            terminal_write("[INFO] Collected child task ID ");
            shell_print_uint((uint32_t)result);
            terminal_putchar('\n');
        } else {
            console_error("Wait failed (not a terminated child)");
        }
    }
    else if (string_starts_with(line, "diskinfo")) {
        if (!ata_is_ready()) {
            console_error("No ATA drive present");
        } else {
            terminal_write("[INFO] Model:  ");
            terminal_write(ata_model()[0] ? ata_model() : "(unreported)");
            terminal_putchar('\n');

            terminal_write("[INFO] Serial: ");
            terminal_write(ata_serial()[0] ? ata_serial() : "(unreported)");
            terminal_putchar('\n');

            terminal_write("[INFO] Capacity: ");
            shell_print_uint((uint32_t)(ata_total_bytes() / 1024U / 1024U));
            terminal_write(" MiB (");
            shell_print_uint((uint32_t)ata_total_sectors());
            terminal_write(" sectors, 512 B each)\n");

            if (ata_requires_lba48()) {
                console_warn("Drive needs LBA48; only the first 128 GiB are addressable");
            }
        }
    }
    else if (string_starts_with(line, "date")) {
        rtc_time_t now;

        if (rtc_read(&now) != 0) {
            console_warn("Hardware clock unavailable; showing fallback date");
        }

        char stamp[32];
        rtc_format(&now, stamp, sizeof(stamp));
        terminal_write(stamp);
        terminal_putchar('\n');
    }
    else if (string_starts_with(line, "storage-test")) {
        if (!fs_is_mounted()) {
            console_error("Storage test requires a formatted filesystem");
        } else if (fs_self_test() != 0) {
            console_error("Storage test failed");
        }
    }
    else if (line[0] == 'f' &&
             line[1] == 'o' &&
             line[2] == 'r' &&
             line[3] == 'm' &&
             line[4] == 'a' &&
             line[5] == 't' &&
             line[6] == '\0') {
        int rc = fs_format(0, 0);

        if (rc == FS_OK) {
            console_info("Disk formatted as LumenFS v2");
        } else {
            console_error("Format failed: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (line[0] == 'l' &&
             line[1] == 's' &&
             line[2] == '\0') {
        shell_list("/");
    }
    else if (string_starts_with(line, "ls ")) {
        const char *path = line + 3;

        while (*path == ' ')
            path++;

        if(path[0]=='/'&&path[1]=='p'&&path[2]=='r'&&path[3]=='o'&&path[4]=='c'&&(path[5]=='\0'||path[5]==' ')){
            terminal_write("meminfo\nversion\nuptime\n");}
        else if(path[0]=='/'&&path[1]=='d'&&path[2]=='e'&&path[3]=='v'&&(path[4]=='\0'||path[4]==' ')){
            terminal_write("null\nzero\n");}
        else shell_list(*path == '\0' ? "/" : path);
    }
    else if (string_starts_with(line, "cat ")) {
        const char *filename = line + 4;

        while (*filename == ' ')
            filename++;

        if (*filename == '\0') {
            console_error("Usage: cat <file>");
        } else {
            void *buffer = 0;
            uint32_t size = 0;
            int rc = fs_read(filename, FS_ROOT, &buffer, &size);

            if (rc != FS_OK) {
                console_error("cat: ");
                terminal_write(fs_strerror(rc));
                terminal_putchar('\n');
            } else {
                if (size > 0) {
                    terminal_write((const char *)buffer);

                    if (((const char *)buffer)[size - 1] != '\n')
                        terminal_putchar('\n');
                }

                kfree(buffer);
            }
        }
    }
    else if (string_starts_with(line, "write ")) {
        const char *rest = line + 6;
        const char *space = 0;

        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                space = p;
                break;
            }
        }

        if (space == 0 || space == rest || space[1] == '\0') {
            console_error("Usage: write <file> <text>");
        } else {
            char filename[FS_NAME_MAX + 1];
            uint32_t name_len = (uint32_t)(space - rest);
            int rc;

            if (name_len > FS_NAME_MAX) {
                console_error("write: name too long");
            } else {
                for (uint32_t i = 0; i < name_len; i++)
                    filename[i] = rest[i];

                filename[name_len] = '\0';

                rc = fs_write(filename, FS_ROOT, space + 1,
                              shell_strlen(space + 1));

                if (rc == FS_OK) {
                    console_info("Wrote ");
                    terminal_write(filename);
                } else {
                    console_error("Write failed: ");
                    terminal_write(fs_strerror(rc));
                    terminal_putchar('\n');
                }
            }
        }
    }
    else if (string_starts_with(line, "rm ")) {
        const char *filename = line + 3;
        int rc = fs_delete(filename, FS_ROOT);

        if (rc == FS_OK) {
            console_info("File deleted");
        } else {
            console_error("rm: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "mkdir ")) {
        const char *path = line + 6;

        while (*path == ' ')
            path++;

        int rc = fs_mkdir(path, FS_ROOT, FS_MODE_DIR_DEFAULT);

        if (rc == FS_OK) {
            console_info("Directory created");
        } else {
            console_error("mkdir: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "rmdir ")) {
        const char *path = line + 6;

        while (*path == ' ')
            path++;

        int rc = fs_rmdir(path, FS_ROOT);

        if (rc == FS_OK) {
            console_info("Directory removed");
        } else {
            console_error("rmdir: ");
            terminal_write(fs_strerror(rc));
            terminal_putchar('\n');
        }
    }
    else if (string_starts_with(line, "ln ")) {
        const char *rest = line + 3;
        int symbolic = 0;

        while (*rest == ' ')
            rest++;

        if (string_starts_with(rest, "-s ")) {
            symbolic = 1;
            rest += 3;
        }

        const char *space = 0;
        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                space = p;
                break;
            }
        }

        if (space == 0 || space == rest || space[1] == '\0') {
            console_error("Usage: ln [-s] <target> <link>");
        } else {
            char target[FS_PATH_MAX];
            char linkpath[FS_PATH_MAX];
            uint32_t target_len = (uint32_t)(space - rest);

            if (target_len >= FS_PATH_MAX) {
                console_error("ln: target path too long");
            } else {
                for (uint32_t i = 0; i < target_len; i++)
                    target[i] = rest[i];
                target[target_len] = '\0';

                const char *linkname = space + 1;
                while (*linkname == ' ')
                    linkname++;

                if (*linkname == '\0') {
                    console_error("Usage: ln [-s] <target> <link>");
                } else {
                    for (uint32_t i = 0; i < FS_PATH_MAX - 1 && linkname[i] != '\0'; i++)
                        linkpath[i] = linkname[i];
                    linkpath[FS_PATH_MAX - 1] = '\0';

                    int rc;
                    if (symbolic) {
                        rc = fs_symlink(target, linkpath, FS_ROOT);
                        if (rc == FS_OK)
                            console_info("Symlink created");
                        else
                            console_error("ln: "), terminal_write(fs_strerror(rc)), terminal_putchar('\n');
                    } else {
                        rc = fs_link(target, linkpath, FS_ROOT);
                        if (rc == FS_OK)
                            console_info("Hard link created");
                        else
                            console_error("ln: "), terminal_write(fs_strerror(rc)), terminal_putchar('\n');
                    }
                }
            }
        }
    }
    else if (string_starts_with(line, "readlink ")) {
        const char *linkpath = line + 9;

        while (*linkpath == ' ')
            linkpath++;

        if (*linkpath == '\0') {
            console_error("Usage: readlink <link>");
        } else {
            char target[256];
            int rc = fs_readlink(linkpath, FS_ROOT, target, sizeof(target));

            if (rc == FS_OK) {
                terminal_write(target);
                terminal_putchar('\n');
            } else {
                console_error("readlink: ");
                terminal_write(fs_strerror(rc));
                terminal_putchar('\n');
            }
        }
    }
    else if (string_starts_with(line, "mv ")) {
        const char *rest = line + 3;

        while (*rest == ' ')
            rest++;

        const char *to = 0;

        for (const char *p = rest; *p != '\0'; p++) {
            if (*p == ' ') {
                to = p + 1;
                break;
            }
        }

        if (to == 0) {
            console_error("Usage: mv <from> <to>");
        } else {
            char from[FS_PATH_MAX];
            uint32_t from_len = (uint32_t)(to - 1 - rest);

            if (from_len >= FS_PATH_MAX) {
                console_error("mv: path too long");
            } else {
                for (uint32_t i = 0; i < from_len; i++)
                    from[i] = rest[i];

                from[from_len] = '\0';

                int rc = fs_rename(from, to, FS_ROOT);

                if (rc == FS_OK) {
                    console_info("Renamed");
                } else {
                    console_error("mv: ");
                    terminal_write(fs_strerror(rc));
                    terminal_putchar('\n');
                }
            }
        }
    }
    else if (string_starts_with(line, "df")) {
        if (!fs_is_mounted()) {
            console_error("No filesystem mounted");
        } else {
            terminal_write("[INFO] Total: ");
            terminal_write_u32((uint32_t)(fs_total_bytes() / 1024U));
            terminal_write(" KiB, free: ");
            terminal_write_u32((uint32_t)(fs_free_bytes() / 1024U));
            terminal_write(" KiB\n");
        }
    }
    else if (line[0] == 'i' &&
             line[1] == 'n' &&
             line[2] == 's' &&
             line[3] == 't' &&
             line[4] == 'a' &&
             line[5] == 'l' &&
             line[6] == 'l' &&
             line[7] == '\0') {

        if (fs_write(
                "hello.bin",
                FS_ROOT,
                ring3_test_program,
                ring3_test_program_size
            ) == FS_OK) {

            console_info("Installed hello.bin");
        } else {
            console_error("Failed to install hello.bin");
        }
    }
    else if (string_starts_with(line, "run ")) {
        const char *filename = line + 4;
        int id = loader_spawn(filename);

        if (id >= 0) {
            terminal_write("[INFO] Spawned task ID ");
            shell_print_uint((uint32_t)id);
            terminal_putchar('\n');
        }
        }
    }
    else if (line[0] == 'j' &&
             line[1] == 'o' &&
             line[2] == 'b' &&
             line[3] == 's' &&
             line[4] == '\0') {
        /* List background jobs. */
        terminal_write("JOB   PID   STATE   CMD\n");
        for (uint32_t id = 1; id <= MAX_TASKS; id++) {
            const task_t *task = task_get(id);
            if (task == 0)
                continue;
            if (task->state == TASK_BLOCKED || task->state == TASK_READY) {
                terminal_write("[");
                shell_print_uint(id);
                terminal_write("] ");
                shell_print_uint(task->id);
                terminal_write(" ");
                if (task->state == TASK_BLOCKED)
                    terminal_write("Stopped");
                else
                    terminal_write("Running");
                terminal_write(" cmd\n");
            }
        }
    }
    else if (line[0] == 'f' &&
             line[1] == 'g' &&
             line[2] == '\0') {
        /* Foreground - resume most recent stopped job. */
        for (int id = MAX_TASKS; id >= 1; id--) {
            const task_t *task = task_get(id);
            if (task && task->state == TASK_BLOCKED) {
                if (task_wake(id) == 0) {
                    terminal_write("[INFO] Resumed job ");
                    shell_print_uint(id);
                    terminal_putchar('\n');
                } else {
                    console_error("Failed to resume job");
                }
                break;
            }
        }
    }
    else if (line[0] == 'b' &&
             line[1] == 'g' &&
             line[2] == '\0') {
        /* Background - resume most recent stopped job in background. */
        for (int id = MAX_TASKS; id >= 1; id--) {
            const task_t *task = task_get(id);
            if (task && task->state == TASK_BLOCKED) {
                if (task_wake(id) == 0) {
                    terminal_write("[INFO] Resumed job ");
                    shell_print_uint(id);
                    terminal_write(" &\n");
                } else {
                    console_error("Failed to resume job");
                }
                break;
            }
        }
    }
    else {
        console_warn("Unknown command");
    }

    shell_prompt();
}
