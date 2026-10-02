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