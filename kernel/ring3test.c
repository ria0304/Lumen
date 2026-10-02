#include <stdint.h>

#include "ring3test.h"
#include "console.h"
#include "fs.h"
#include "heap.h"
#include "kmem.h"
#include "paging.h"
#include "privilege.h"
#include "scheduler.h"
#include "task.h"
#include "loader.h"
#include "uaccess.h"

/*
 * The probe and its size are defined in kernel/ring3test.asm, which
 * is assembled into the kernel image. ring3_io_size is the exact byte
 * count, computed by the assembler.
 */
extern uint8_t ring3_io_entry[];
extern uint32_t ring3_io_size;
extern uint8_t ring3_fault_entry[];
extern uint32_t ring3_fault_size;
extern uint8_t ring3_badop_entry[];
extern uint32_t ring3_badop_size;
extern uint8_t ring3_fork_entry[];
extern uint32_t ring3_fork_size;
extern uint8_t ring3_exec_entry[];
extern uint32_t ring3_exec_size;
extern uint8_t ring3_execprog[];
extern uint32_t ring3_execprog_len;
extern const unsigned char lumen_elfprog[];
extern const unsigned int lumen_elfprog_len;
extern uint8_t ring3_syscalls_entry[];
extern uint32_t ring3_syscalls_size;

extern volatile uint32_t timer_ticks;

#define R3_DIR  "/tmp"
#define R3_FILE R3_DIR "/io.txt"

#define R3_PATH_ADDR     0x01001000U
#define R3_DATA_ADDR     0x01001040U
#define R3_RES_BASE      0x010010C0U

#define RES_OPEN1     (R3_RES_BASE + 0x00)
#define RES_WRITE     (R3_RES_BASE + 0x04)
#define RES_CLOSE1    (R3_RES_BASE + 0x08)
#define RES_OPEN2     (R3_RES_BASE + 0x0C)
#define RES_READ      (R3_RES_BASE + 0x10)
#define RES_CMP       (R3_RES_BASE + 0x14)
#define RES_BADWRITE  (R3_RES_BASE + 0x18)
#define RES_BADREAD   (R3_RES_BASE + 0x1C)
#define RES_BADOPEN   (R3_RES_BASE + 0x20)
#define RES_GETPID    (R3_RES_BASE + 0x24)
#define RES_YIELD     (R3_RES_BASE + 0x28)
#define RES_CLOSE2    (R3_RES_BASE + 0x2C)
#define RES_SHORTREAD (R3_RES_BASE + 0x34)
#define RES_FORK_PID  (R3_RES_BASE + 0x40)
#define RES_FORK_CHILD (R3_RES_BASE + 0x44)
#define RES_FORK_PPID (R3_RES_BASE + 0x48)
#define RES_FORK_PPID2 (R3_RES_BASE + 0x4C)
#define RES_FORK_WAIT (R3_RES_BASE + 0x50)
#define RES_FORK_DONE (R3_RES_BASE + 0x54)
#define FORK_DONE_MAGIC 0x00C0FFEEU
#define RES_FORK_PARENTBUF (R3_RES_BASE + 0x58)
#define RES_FORK_CHILDBUF  (R3_RES_BASE + 0x5C)
#define RES_EXEC_RC        (R3_RES_BASE + 0x60)

#define RES_SC_UID      (R3_RES_BASE + 0x70)
#define RES_SC_GID      (R3_RES_BASE + 0x74)
#define RES_SC_PID      (R3_RES_BASE + 0x78)
#define RES_SC_PGID     (R3_RES_BASE + 0x7C)
#define RES_SC_SETSID   (R3_RES_BASE + 0x80)
#define RES_SC_MASK     (R3_RES_BASE + 0x84)
#define RES_SC_SIGNAL   (R3_RES_BASE + 0x88)
#define RES_SC_SIGBAD   (R3_RES_BASE + 0x8C)
#define RES_SC_SIGRANGE (R3_RES_BASE + 0x90)
#define RES_SC_DUP2BAD  (R3_RES_BASE + 0x94)
#define RES_SC_CLOSEBAD (R3_RES_BASE + 0x98)
#define RES_SC_KILLBAD  (R3_RES_BASE + 0x9C)
#define RES_SC_UNKNOWN  (R3_RES_BASE + 0xA0)
#define RES_SC_PID2     (R3_RES_BASE + 0xA4)

#define R3_FAIL_MARK 0xDEADBEEFU

/* What the probe is supposed to write. */
static const uint8_t expect_payload[8] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'
};

/*
 * Read one result word out of the task's address space. The probe has
 * already terminated by the time we do this, but its page directory
 * is still valid until task_wait() reclaims it.
 */
static uint32_t r3_read(uint32_t dir, uint32_t uaddr)
{
    uint32_t value = R3_FAIL_MARK;

    if (uaccess_read_u32(dir, uaddr, &value) != 0)
        return R3_FAIL_MARK;

    return value;
}

int ring3_io_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    if (!fs_is_mounted()) {
        console_warn("RING3 self-test: no filesystem, skipping");
        return 1;
    }

    /* The probe writes into /tmp, so make sure it is there. */
    fs_mkdir(R3_DIR, FS_ROOT, FS_MODE_DIR_DEFAULT);
    fs_delete(R3_FILE, FS_ROOT);

    int id = task_create_user_program(ring3_io_entry, ring3_io_size);

    if (id < 0) {
        console_error("RING3 self-test: could not create the task");
        return 0;
    }

    const task_t *probe = task_get((uint32_t)id);

    if (probe == 0) {
        task_wait((uint32_t)id);
        return 0;
    }

    uint32_t dir = probe->page_directory;

    /* The probe must really be a user task, or nothing it does proves
     * anything about Ring 3. */
    CHECK(probe->privilege == USER_RING, "task is not Ring 3");

    /* Let it run to completion, with a bound so a wedged task cannot
     * hang the boot. */
    uint32_t start = timer_ticks;

    for (;;) {
        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 2000)
            break;

        __asm__ volatile ("hlt");
    }

    const task_t *done = task_get((uint32_t)id);

    if (done == 0 || done->state != TASK_TERMINATED) {
        console_error("RING3 self-test: the task never terminated");
        task_terminate((uint32_t)id);
        task_wait((uint32_t)id);
        return 0;
    }

    /* What the probe saw. */
    CHECK(r3_read(dir, RES_OPEN1) != R3_FAIL_MARK &&
          (int32_t)r3_read(dir, RES_OPEN1) >= 0, "create open failed");

    CHECK(r3_read(dir, RES_WRITE) == 8, "Ring 3 write did not report 8 bytes");

    CHECK(r3_read(dir, RES_CLOSE1) == 0, "Ring 3 close failed");

    CHECK((int32_t)r3_read(dir, RES_OPEN2) >= 0, "reopen failed");

    CHECK(r3_read(dir, RES_READ) == 8, "Ring 3 read did not report 8 bytes");

    CHECK(r3_read(dir, RES_CMP) == 1, "Ring 3 read back different bytes");

    CHECK(r3_read(dir, RES_SHORTREAD) == 0, "read past EOF did not report 0");

    CHECK((int32_t)r3_read(dir, RES_BADWRITE) == -1,
          "write with a kernel pointer was allowed");

    CHECK((int32_t)r3_read(dir, RES_BADREAD) == -1,
          "read with a kernel pointer was allowed");

    CHECK((int32_t)r3_read(dir, RES_BADOPEN) == -1,
          "open with a kernel pointer was allowed");

    CHECK((int32_t)r3_read(dir, RES_GETPID) >= 1, "getpid looks wrong");

    CHECK(r3_read(dir, RES_YIELD) == 0, "yield did not report success");

    CHECK(r3_read(dir, RES_CLOSE2) == 0, "second close failed");

    /*
     * The important part: what the task wrote must be readable from
     * the kernel side too. A Ring 3 write that reported success but
     * landed in the wrong place passes every check above and is only
     * caught here.
     */
    {
        void *back = 0;
        uint32_t size = 0;
        int rc = fs_read(R3_FILE, FS_ROOT, &back, &size);

        CHECK(rc == FS_OK, "the file the task wrote is not readable");
        CHECK(size == sizeof(expect_payload), "the file has the wrong size");

        if (back != 0) {
            CHECK(memcmp(back, expect_payload, sizeof(expect_payload)) == 0,
                  "the file does not hold what the task wrote");

            kfree(back);
        }
    }

    task_wait((uint32_t)id);

    fs_delete(R3_FILE, FS_ROOT);

#undef CHECK

    return failures == 0;
}

/*
 * fork() from Ring 3. SYS_FORK is dispatched to task_fork_user() for
 * a user caller, which clones the address space and gives the child a
 * copy of the interrupted frame so the child resumes after the syscall
 * with EAX == 0. Both the parent's view (a positive child id, then a
 * successful wait) and the child's view (fork() == 0, and a parent
 * that is the task we forked from) are checked.
 */
int ring3_fork_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    int id = task_create_user_program(ring3_fork_entry, ring3_fork_size);

    if (id < 0) {
        console_error("RING3 self-test: could not create the fork probe");
        return 0;
    }

    const task_t *probe = task_get((uint32_t)id);
    uint32_t dir = probe != 0 ? probe->page_directory : 0;
    uint32_t parent_pid = probe != 0 ? probe->id : 0;

    /*
     * Phase 1: wait for the child to publish its results. It stays
     * alive afterwards, because the parent's SYS_WAIT would release
     * the very pages we need to read.
     */
    uint32_t child_pid = 0;
    uint32_t child_fork_return = 0xDEADBEEFU;
    uint32_t child_ppid = 0xDEADBEEFU;
    uint32_t child_buf = 0xDEADBEEFU;
    int child_seen = 0;
    uint32_t start = timer_ticks;

    for (;;) {
        uint32_t pid = r3_read(dir, RES_FORK_PID);

        if (pid != 0xDEADBEEFU && (int32_t)pid > 0) {
            const task_t *c = task_get(pid);

            if (c != 0) {
                child_pid = pid;

                if (r3_read(c->page_directory, RES_FORK_DONE)
                    == FORK_DONE_MAGIC) {

                    CHECK(c->page_directory != dir,
                          "the child shares the parent's address space");

                    child_fork_return =
                        r3_read(c->page_directory, RES_FORK_CHILD);
                    child_ppid = r3_read(c->page_directory, RES_FORK_PPID);
                    child_buf =
                        r3_read(c->page_directory, RES_FORK_CHILDBUF);

                    CHECK((int32_t)child_ppid == (int32_t)parent_pid,
                          "the child's parent id is wrong");
                    CHECK(child_buf == 0xBBBBBBBBU,
                          "the child did not see its own write");

                    child_seen = 1;
                    break;
                }
            }
        }

        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 3000) {
            console_error("RING3 self-test: the fork child never "
                          "published its results");
            break;
        }

        __asm__ volatile ("hlt");
    }

    CHECK(child_seen, "the child never published its results");
    CHECK(child_fork_return == 0, "the child did not see fork() return 0");
    CHECK((int32_t)child_pid > 0, "fork did not return a child id");

    /* Retire the child so the parent's blocking-free wait can finish. */
    if (child_seen)
        task_terminate(child_pid);

    /*
     * Phase 2: the parent is spinning in its yield/wait loop; it
     * completes once the child is gone.
     */
    for (;;) {
        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 6000) {
            console_error("RING3 self-test: the fork parent never "
                          "finished waiting");
            break;
        }

        __asm__ volatile ("hlt");
    }

    const task_t *done = task_get((uint32_t)id);

    CHECK(done != 0 && done->state == TASK_TERMINATED,
          "the fork probe did not terminate");

    CHECK((int32_t)r3_read(dir, RES_FORK_WAIT) == (int32_t)child_pid,
          "wait did not return the child id");

    /*
     * The decisive check: the child overwrote a page both tasks
     * inherited, and the parent's copy must be exactly as it left it.
     */
    CHECK(r3_read(dir, RES_FORK_PARENTBUF) == 0xAAAAAAAAU,
          "the child's writes reached the parent's memory");

    if ((int32_t)child_pid > 0)
        task_wait(child_pid);

    task_wait((uint32_t)id);

#undef CHECK

    return failures == 0;
}
static int ring3_expect_fault(const char *what,
                              const uint8_t *code,
                              uint32_t size)
{
    int id = task_create_user_program(code, size);

    if (id < 0) {
        console_error("RING3 self-test: could not create the ");
        terminal_write(what);
        console_error(" probe");
        return 0;
    }

    uint32_t start = timer_ticks;

    for (;;) {
        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 2000) {
            console_error("RING3 self-test: ");
            terminal_write(what);
            console_error(" probe never faulted");
            task_terminate((uint32_t)id);
            task_wait((uint32_t)id);
            return 0;
        }

        __asm__ volatile ("hlt");
    }

    const task_t *done = task_get((uint32_t)id);

    if (done == 0 || done->state != TASK_TERMINATED) {
        console_error("RING3 self-test: ");
        terminal_write(what);
        console_error(" probe was not retired by its fault");
        task_wait((uint32_t)id);
        return 0;
    }

    task_wait((uint32_t)id);

    return 1;
}

/*
 * Run one program that is expected to fault, and check that the fault
 * retired it without taking the machine with it.
 */

/*
 * Fault isolation. Two programs that fault for different reasons must
 * each be retired on their own, and the machine must still be healthy
 * afterwards -- proven by running a well-behaved Ring 3 task after
 * them and requiring it to complete.
 */
int ring3_fault_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    if (!fs_is_mounted()) {
        console_warn("RING3 self-test: no filesystem, skipping fault checks");
        return 1;
    }

    CHECK(ring3_expect_fault("page-fault", ring3_fault_entry,
                             ring3_fault_size),
          "an unmapped Ring 3 read was not isolated");

    CHECK(ring3_expect_fault("invalid-opcode", ring3_badop_entry,
                             ring3_badop_size),
          "an invalid opcode in Ring 3 was not isolated");

    /*
     * The machine must still work. Running the full I/O probe again
     * after the faults is the real assertion: it exercises paging, the
     * scheduler and the filesystem once more, so a corrupted frame
     * table or a wedged scheduler shows up here.
     */
    {
        fs_mkdir(R3_DIR, FS_ROOT, FS_MODE_DIR_DEFAULT);
        fs_delete(R3_FILE, FS_ROOT);

        int id = task_create_user_program(ring3_io_entry, ring3_io_size);

        CHECK(id >= 0, "a healthy Ring 3 task would not start after the faults");

        if (id >= 0) {
            const task_t *probe = task_get((uint32_t)id);
            uint32_t dir = probe != 0 ? probe->page_directory : 0;
            uint32_t start = timer_ticks;

            for (;;) {
                const task_t *t = task_get((uint32_t)id);

                if (t == 0 || t->state == TASK_TERMINATED)
                    break;

                if (timer_ticks - start > 2000)
                    break;

                __asm__ volatile ("hlt");
            }

            const task_t *done = task_get((uint32_t)id);

            CHECK(done != 0 && done->state == TASK_TERMINATED,
                  "the post-fault task did not finish");

            if (dir != 0) {
                CHECK(r3_read(dir, RES_WRITE) == 8,
                      "the post-fault task could not write");
                CHECK(r3_read(dir, RES_CMP) == 1,
                      "the post-fault task read back wrong bytes");
            }

            task_wait((uint32_t)id);
        }

        fs_delete(R3_FILE, FS_ROOT);
    }

#undef CHECK

    return failures == 0;
}

/*
 * exec() end to end.
 *
 * The launcher task execs a program from LumenFS, which replaces its
 * own image. The loaded program then writes a file, so a pass means the
 * whole chain worked: the image was read, mapped, entered at Ring 3,
 * and could do file I/O from its brand new address space.
 */
int ring3_exec_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    if (!fs_is_mounted()) {
        console_warn("RING3 self-test: no filesystem, skipping exec");
        return 1;
    }

    static const char prog_path[] = R3_DIR "/prog.bin";
    static const char out_path[]  = R3_DIR "/prog.out";

    fs_mkdir(R3_DIR, FS_ROOT, FS_MODE_DIR_DEFAULT);
    fs_delete(prog_path, FS_ROOT);
    fs_delete(out_path, FS_ROOT);

    /* Put the program to be exec'd on the filesystem. */
    CHECK(fs_write(prog_path, FS_ROOT, ring3_execprog, ring3_execprog_len)
              == FS_OK,
          "could not stage the exec'd program");

    int id = task_create_user_program(ring3_exec_entry, ring3_exec_size);

    if (id < 0) {
        console_error("RING3 self-test: could not create the exec launcher");
        return 0;
    }

    const task_t *probe = task_get((uint32_t)id);
    uint32_t dir = probe != 0 ? probe->page_directory : 0;
    uint32_t start = timer_ticks;

    for (;;) {
        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 3000) {
            console_error("RING3 self-test: the exec launcher never "
                          "finished");
            break;
        }

        __asm__ volatile ("hlt");
    }

    /*
     * There is deliberately no check on the launcher's SYS_EXEC
     * return value: exec replaces the caller's image, so the caller
     * never gets to store a result. The file below is the proof.
     */
    (void)dir;

    /*
     * The real proof: the program that replaced the launcher's image
     * wrote this. Nothing in the kernel produced it.
     */
    {
        void *back = 0;
        uint32_t size = 0;

        int rc = fs_read(out_path, FS_ROOT, &back, &size);

        CHECK(rc == FS_OK, "the exec'd program wrote no output file");
        CHECK(size == 7, "the output file has the wrong size");

        if (back != 0) {
            CHECK(memcmp(back, "EXEC_OK", 7) == 0,
                  "the exec'd program wrote the wrong contents");

            kfree(back);
        }
    }

    task_wait((uint32_t)id);

    fs_delete(prog_path, FS_ROOT);
    fs_delete(out_path, FS_ROOT);

#undef CHECK

    return failures == 0;
}

/*
 * Syscall coverage and error-path checks.
 *
 * The point is not that each syscall exists, but that the dispatcher
 * rejects bad arguments: out-of-range signal numbers, file
 * descriptors and pids, kernel pointers passed as signal handlers, and
 * syscall numbers it does not implement. A dispatcher that falls
 * through leaves frame[7] holding the syscall number, so a user task
 * sees a garbage "success" from every call it does not recognise.
 */
int ring3_syscalls_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    int id = task_create_user_program(ring3_syscalls_entry,
                                      ring3_syscalls_size);

    if (id < 0) {
        console_error("RING3 self-test: could not create the syscall probe");
        return 0;
    }

    const task_t *probe = task_get((uint32_t)id);
    uint32_t dir = probe != 0 ? probe->page_directory : 0;
    uint32_t start = timer_ticks;

    for (;;) {
        const task_t *t = task_get((uint32_t)id);

        if (t == 0 || t->state == TASK_TERMINATED)
            break;

        if (timer_ticks - start > 2000) {
            console_error("RING3 self-test: the syscall probe never "
                          "finished");
            break;
        }

        __asm__ volatile ("hlt");
    }

    const task_t *done = task_get((uint32_t)id);

    CHECK(done != 0 && done->state == TASK_TERMINATED,
          "the syscall probe did not terminate");

    uint32_t pid = r3_read(dir, RES_SC_PID);

    /* Identity and credentials. */
    CHECK(r3_read(dir, RES_SC_UID) == 0, "getuid did not report 0");
    CHECK(r3_read(dir, RES_SC_GID) == 0, "getgid did not report 0");
    CHECK((int32_t)pid >= 1, "getpid looks wrong");
    CHECK(r3_read(dir, RES_SC_PGID) == pid, "getpgid != pid");
    CHECK(r3_read(dir, RES_SC_SETSID) == pid, "setsid did not return the pid");

    /* Signals. */
    CHECK(r3_read(dir, RES_SC_MASK) == 0, "sigprocmask failed");
    CHECK(r3_read(dir, RES_SC_SIGNAL) == 0,
          "signal with a user handler was refused");
    CHECK((int32_t)r3_read(dir, RES_SC_SIGBAD) == -1,
          "signal accepted a kernel pointer as a handler");
    CHECK((int32_t)r3_read(dir, RES_SC_SIGRANGE) == -1,
          "signal accepted an out-of-range signal number");

    /* Descriptor and pid validation. */
    CHECK((int32_t)r3_read(dir, RES_SC_DUP2BAD) == -1,
          "dup2 accepted an out-of-range descriptor");
    CHECK((int32_t)r3_read(dir, RES_SC_CLOSEBAD) == -1,
          "close accepted an out-of-range descriptor");
    CHECK((int32_t)r3_read(dir, RES_SC_KILLBAD) == -1,
          "kill accepted an out-of-range pid");

    /* An unimplemented syscall must fail rather than look successful. */
    CHECK((int32_t)r3_read(dir, RES_SC_UNKNOWN) == -1,
          "an unknown syscall did not fail");

    /* And the task must still work after all of that. */
    CHECK(r3_read(dir, RES_SC_PID2) == pid,
          "the task was disturbed by the failed syscalls");

    task_wait((uint32_t)id);

#undef CHECK

    return failures == 0;
}

/*
 * The ELF loader, end to end.
 *
 * Unlike RING3EXE, which hands the loader raw bytes, this stages a
 * genuine ELF32 executable built by a real toolchain, so the header
 * parsing and PT_LOAD mapping are actually exercised. The program
 * writes /tmp/elf.ok and exits; nothing in the kernel creates that
 * file, so finding it proves the image was parsed, mapped at its own
 * virtual address, entered at CPL 3, and able to do file I/O.
 */
int ring3_elf_run_self_test(void)
{
    int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            console_error("RING3 self-test: " msg);                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

    static const char prog_path[] = R3_DIR "/prog.elf";
    static const char out_path[]  = R3_DIR "/elf.ok";

    fs_mkdir(R3_DIR, FS_ROOT, FS_MODE_DIR_DEFAULT);
    fs_delete(prog_path, FS_ROOT);
    fs_delete(out_path, FS_ROOT);

    CHECK(fs_write(prog_path, FS_ROOT, lumen_elfprog, lumen_elfprog_len)
              == FS_OK,
          "could not stage the ELF");

    int id = loader_spawn_elf(prog_path);

    CHECK(id >= 0, "the ELF loader refused a valid executable");

    if (id >= 0) {
        uint32_t start = timer_ticks;

        for (;;) {
            const task_t *t = task_get((uint32_t)id);

            if (t == 0 || t->state == TASK_TERMINATED)
                break;

            if (timer_ticks - start > 3000) {
                console_error("RING3 self-test: the ELF program never "
                              "finished");
                break;
            }

            __asm__ volatile ("hlt");
        }

        const task_t *done = task_get((uint32_t)id);

        CHECK(done != 0 && done->state == TASK_TERMINATED,
              "the ELF program did not terminate");

        void *back = 0;
        uint32_t size = 0;

        CHECK(fs_read(out_path, FS_ROOT, &back, &size) == FS_OK,
              "the ELF program wrote no output file");
        CHECK(size == 7, "the ELF output file has the wrong size");

        if (back != 0) {
            CHECK(memcmp(back, "ELFDONE", 7) == 0,
                  "the ELF program wrote the wrong contents");

            kfree(back);
        }

        task_wait((uint32_t)id);
    }

    fs_delete(prog_path, FS_ROOT);
    fs_delete(out_path, FS_ROOT);

#undef CHECK

    return failures == 0;
}
