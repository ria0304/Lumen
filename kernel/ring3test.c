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