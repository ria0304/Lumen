#include <stdint.h>
#include "task.h"
#include "console.h"
#include "frame.h"
#include "paging.h"
#include "ring3.h"
#include "fs.h"
#include "kmem.h"
#include "privilege.h"
#include "heap.h"
#include "elf.h"

extern uint32_t scheduler_current_task(void);

extern void task_demo_entry(void);

static task_t tasks[MAX_TASKS + 1];

/*
 * Fixed kernel stacks live in BSS. Every task, kernel-ring or
 * user-ring, gets a permanently dedicated 4 KiB stack here. For
 * Ring 3 tasks this doubles as the TSS.esp0 target while that
 * task is current -- each task's interrupt/syscall entry frame
 * lands on ITS OWN stack, never aliasing task 0's or any other
 * task's.
 */
static uint8_t task_stacks[MAX_TASKS + 1][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

static uint32_t active_tasks = 0;

/* Pipe implementation. */
#define PIPE_BUFFER_SIZE 4096

typedef struct {
    uint8_t buffer[PIPE_BUFFER_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t size;
    int readers;
    int writers;
    int closed;
} pipe_t;

#define MAX_PIPES 16
static pipe_t pipes[MAX_PIPES];

int pipe_create(void)
{
    for (uint32_t i = 0; i < MAX_PIPES; i++) {
        if (pipes[i].readers == 0 && pipes[i].writers == 0) {
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].size = 0;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            pipes[i].closed = 0;
            return (int)i;
        }
    }
    return -1;
}

int pipe_read(int pipe_fd, void *buffer, uint32_t size, uint32_t *out_read)
{
    if (pipe_fd >= MAX_PIPES || pipe_fd < 0)
        return -1;

    pipe_t *pipe = &pipes[pipe_fd];
    
    if (pipe->readers == 0)
        return -1;

    uint32_t read = 0;
    while (read < size) {
        if (pipe->size == 0) {
            if (pipe->writers == 0) {
                /* EOF */
                break;
            }
            /* Wait for data - in a real implementation we'd block */
            /* For now, just return what we have */
            break;
        }

        uint32_t chunk = pipe->size;
        if (chunk > size - read)
            chunk = size - read;

        uint8_t *dst = (uint8_t *)buffer + read;
        uint32_t read_pos = pipe->read_pos;

        if (read_pos + chunk <= PIPE_BUFFER_SIZE) {
            for (uint32_t i = 0; i < chunk; i++)
                dst[i] = pipes[pipe_fd].buffer[read_pos + i];
        } else {
            uint32_t first_chunk = PIPE_BUFFER_SIZE - read_pos;
            for (uint32_t i = 0; i < first_chunk; i++)
                dst[i] = pipes[pipe_fd].buffer[read_pos + i];
            for (uint32_t i = 0; i < chunk - first_chunk; i++)
                dst[first_chunk + i] = pipes[pipe_fd].buffer[i];
        }

        pipe->read_pos = (pipe->read_pos + chunk) % PIPE_BUFFER_SIZE;
        pipe->size -= chunk;
        read += chunk;
    }

    if (out_read)
        *out_read = read;
    return (read > 0) ? 0 : -1;
}

int pipe_write(int pipe_fd, const void *buffer, uint32_t size, uint32_t *out_written)
{
    if (pipe_fd >= MAX_PIPES || pipe_fd < 0)
        return -1;

    pipe_t *pipe = &pipes[pipe_fd];
    
    if (pipe->writers == 0)
        return -1;

    uint32_t written = 0;
    while (written < size) {
        if (pipe->size >= PIPE_BUFFER_SIZE) {
            /* Buffer full - wait for space */
            break;
        }

        uint32_t free_space = PIPE_BUFFER_SIZE - pipe->size;
        uint32_t chunk = size - written;
        if (chunk > free_space)
            chunk = free_space;

        uint8_t *src = (uint8_t *)buffer + written;
        uint32_t write_pos = pipe->write_pos;

        if (write_pos + chunk <= PIPE_BUFFER_SIZE) {
            for (uint32_t i = 0; i < chunk; i++)
                pipe->buffer[write_pos + i] = src[i];
        } else {
            uint32_t first_chunk = PIPE_BUFFER_SIZE - write_pos;
            for (uint32_t i = 0; i < first_chunk; i++)
                pipe->buffer[write_pos + i] = src[i];
            for (uint32_t i = 0; i < chunk - first_chunk; i++)
                pipe->buffer[i] = src[first_chunk + i];
        }

        pipe->write_pos = (pipe->write_pos + chunk) % PIPE_BUFFER_SIZE;
        pipe->size += chunk;
        written += chunk;
    }

    if (out_written)
        *out_written = written;
    return (written > 0) ? 0 : -1;
}

int pipe_close_read(int pipe_fd)
{
    if (pipe_fd >= MAX_PIPES || pipe_fd < 0)
        return -1;

    pipe_t *pipe = &pipes[pipe_fd];
    if (pipe->readers == 0)
        return -1;

    pipe->readers--;
    if (pipe->readers == 0 && pipe->writers == 0) {
        /* Pipe fully closed, clean up */
        pipe->size = 0;
        pipe->read_pos = 0;
        pipe->write_pos = 0;
    }
    return 0;
}

int pipe_close_write(int pipe_fd)
{
    if (pipe_fd >= MAX_PIPES || pipe_fd < 0)
        return -1;

    pipe_t *pipe = &pipes[pipe_fd];
    if (pipe->writers == 0)
        return -1;

    pipe->writers--;
    if (pipe->writers == 0 && pipe->readers == 0) {
        pipe->size = 0;
        pipe->read_pos = 0;
        pipe->write_pos = 0;
    }
    return 0;
}


static uint32_t task_prepare_stack(
    uint32_t stack_base,
    uint32_t stack_size,
    uint32_t entry
)
{
    uint32_t *sp =
        (uint32_t *)(stack_base + stack_size);

    /*
     * Final interrupt-return layout:
     *
     * EDI
     * ESI
     * EBP
     * ESP dummy
     * EBX
     * EDX
     * ECX
     * EAX
     * EIP
     * CS
     * EFLAGS
     */
    *--sp = 0x00000202;
    *--sp = KERNEL_CODE_SELECTOR;
    *--sp = entry;

    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    return (uint32_t)sp;
}

/*
 * Same idea, but for a Ring 3 task's FIRST entry. IRET detects
 * that the CS being restored (USER_CODE_SELECTOR, RPL=3) differs
 * from the current CPL and automatically pops the extra ESP/SS
 * pair too, performing the Ring 0 -> Ring 3 transition. No
 * special-cased assembly is needed for this -- the existing
 * irq0 popa/iret path in isr.asm handles both frame shapes.
 *
 * Final layout:
 *
 * EDI
 * ESI
 * EBP
 * ESP dummy
 * EBX
 * EDX
 * ECX
 * EAX
 * EIP
 * CS
 * EFLAGS
 * ESP (user)
 * SS  (user)
 */
static uint32_t task_prepare_user_stack(
    uint32_t stack_base,
    uint32_t stack_size,
    uint32_t entry,
    uint32_t user_esp
)
{
    uint32_t *sp =
        (uint32_t *)(stack_base + stack_size);

    *--sp = USER_DATA_SELECTOR;
    *--sp = user_esp;
    *--sp = 0x00000202;
    *--sp = USER_CODE_SELECTOR;
    *--sp = entry;

    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    return (uint32_t)sp;
}

void task_init(void)
{
    for (uint32_t i = 0; i <= MAX_TASKS; i++) {
        tasks[i].id = 0;
        tasks[i].parent_id = 0;
        tasks[i].state = TASK_UNUSED;
        tasks[i].privilege = KERNEL_RING;
        tasks[i].page_directory = PAGE_DIRECTORY_ADDRESS;

        tasks[i].stack_base = 0;
        tasks[i].stack_size = 0;
        tasks[i].switch_esp = 0;

        tasks[i].context.eax = 0;
        tasks[i].context.ebx = 0;
        tasks[i].context.ecx = 0;
        tasks[i].context.edx = 0;
        tasks[i].context.esi = 0;
        tasks[i].context.edi = 0;
        tasks[i].context.ebp = 0;
        tasks[i].context.esp = 0;
        tasks[i].context.eip = 0;
        tasks[i].context.eflags = 0x202;

        for (int j = 0; j < MAX_FDS; j++) {
            tasks[i].fd_table.open[j] = 0;
        }
        tasks[i].cwd_inode = 0;  /* root directory inode */

        tasks[i].signals.pending = 0;
        tasks[i].signals.mask = 0;
        for (int s = 0; s < 32; s++) {
            tasks[i].signals.handlers[s] = 0;
        }
    }

    /*
     * Task 0 represents the kernel/shell execution context.
     * It never faults from Ring 3, so its own stack_base/size
     * (and whatever TSS.esp0 happens to hold while it runs) are
     * never actually used by hardware -- they only matter for
     * tasks that DO take a privilege-changing interrupt.
     */
    tasks[0].id = 0;
    tasks[0].parent_id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].privilege = KERNEL_RING;
    tasks[0].page_directory = PAGE_DIRECTORY_ADDRESS;
    tasks[0].stack_base = 0x90000;
    tasks[0].stack_size = 0x10000;

    active_tasks = 0;

    console_info("Task manager initialized: OK");
}

/*
 * Shared Ring 3 task setup: builds an isolated address space,
 * copies 'code' (up to PAGE_SIZE bytes) into a dedicated code
 * frame, gives it a dedicated zeroed stack frame, and prepares
 * the interrupt-return frame that lands it in Ring 3 at
 * TASK_RING3_CODE_VA. Used by both the fixed-demo path
 * (task_create_with_privilege(USER_RING)) and the general
 * loader path (task_create_user_program()) so there is exactly
 * one place that gets this sequence right.
 */
static int task_create_ring3_with_code(
    const uint8_t *code,
    uint32_t code_size
)
{
    if (code_size > PAGE_SIZE)
        return -1;

    uint32_t i;

    for (i = 1; i <= MAX_TASKS; i++) {

        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_TERMINATED)
            break;
    }

    if (i > MAX_TASKS)
        return -1;

    tasks[i].id = i;
    tasks[i].parent_id = scheduler_current_task();
    tasks[i].state = TASK_READY;
    tasks[i].privilege = USER_RING;

    tasks[i].stack_base = (uint32_t)&task_stacks[i][0];
    tasks[i].stack_size = TASK_STACK_SIZE;

    tasks[i].context.eax = 0;
    tasks[i].context.ebx = 0;
    tasks[i].context.ecx = 0;
    tasks[i].context.edx = 0;
    tasks[i].context.esi = 0;
    tasks[i].context.edi = 0;
    tasks[i].context.eflags = 0x202;

    __asm__ volatile ("cli");

    uint32_t directory = paging_create_address_space();

    if (directory == FRAME_INVALID) {
        console_error("User task: directory FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint32_t code_phys = frame_alloc();

    if (code_phys == FRAME_INVALID ||
        code_phys >= PAGING_IDENTITY_LIMIT) {

        if (code_phys != FRAME_INVALID)
            frame_free(code_phys);

        frame_free(directory);
        console_error("User task: code frame FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint8_t *code_dst = (uint8_t *)code_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        code_dst[k] = (k < code_size) ? code[k] : 0;

    if (paging_map_in_directory(
            directory,
            TASK_RING3_CODE_VA,
            code_phys,
            PAGE_PRESENT | PAGE_USER
        ) != 0) {

        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: code map FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint32_t stack_phys = frame_alloc();

    if (stack_phys == FRAME_INVALID ||
        stack_phys >= PAGING_IDENTITY_LIMIT) {

        if (stack_phys != FRAME_INVALID)
            frame_free(stack_phys);

        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: stack frame FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    uint8_t *stack_dst = (uint8_t *)stack_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        stack_dst[k] = 0;

    if (paging_map_in_directory(
            directory,
            TASK_RING3_STACK_VA,
            stack_phys,
            PAGE_PRESENT | PAGE_WRITE | PAGE_USER
        ) != 0) {

        frame_free(stack_phys);
        frame_free(code_phys);
        frame_free(directory);
        console_error("User task: stack map FAILED");
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    tasks[i].page_directory = directory;

    for (int j = 0; j < MAX_FDS; j++) {
        tasks[i].fd_table.open[j] = 0;
    }
    tasks[i].cwd_inode = tasks[0].cwd_inode;

    tasks[i].signals.pending = 0;
    tasks[i].signals.mask = 0;
    for (int s = 0; s < 32; s++) {
        tasks[i].signals.handlers[s] = 0;
    }

    tasks[i].switch_esp =
        task_prepare_user_stack(
            tasks[i].stack_base,
            tasks[i].stack_size,
            TASK_RING3_CODE_VA,
            TASK_RING3_STACK_TOP
        );

    __asm__ volatile ("sti");

    active_tasks++;

    return (int)i;
}

int task_create_with_privilege(uint32_t privilege)
{
    if (privilege != KERNEL_RING &&
        privilege != USER_RING)
        return -1;

    for (uint32_t i = 1; i <= MAX_TASKS; i++) {

        if (tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_TERMINATED)
            continue;

        tasks[i].id = i;
        tasks[i].parent_id = scheduler_current_task();
        tasks[i].state = TASK_READY;
        tasks[i].privilege = privilege;
        tasks[i].page_directory = PAGE_DIRECTORY_ADDRESS;

        tasks[i].stack_base =
            (uint32_t)&task_stacks[i][0];

        tasks[i].stack_size = TASK_STACK_SIZE;

        tasks[i].context.eax = 0;
        tasks[i].context.ebx = 0;
        tasks[i].context.ecx = 0;
        tasks[i].context.edx = 0;
        tasks[i].context.esi = 0;
        tasks[i].context.edi = 0;

        tasks[i].context.esp =
            tasks[i].stack_base + TASK_STACK_SIZE;

        tasks[i].context.ebp =
            tasks[i].stack_base + TASK_STACK_SIZE;

        tasks[i].context.eip =
            (uint32_t)task_demo_entry;

        tasks[i].context.eflags = 0x202;

        if (privilege == KERNEL_RING) {

            tasks[i].signals.pending = 0;
            tasks[i].signals.mask = 0;
            for (int s = 0; s < 32; s++) {
                tasks[i].signals.handlers[s] = 0;
            }

            tasks[i].switch_esp =
                task_prepare_stack(
                    tasks[i].stack_base,
                    tasks[i].stack_size,
                    (uint32_t)task_demo_entry
                );

        } else {

            __asm__ volatile ("cli");

            /*
             * Build a dedicated, isolated address space: its
             * own page directory, its own physical code/stack
             * frames, mapped only inside that directory. If
             * anything here fails, unwind everything already
             * allocated and refuse to create the task rather
             * than leave it half-built.
             */
            uint32_t directory =
                paging_create_address_space();

            if (directory == FRAME_INVALID) {
                console_error("Ring 3 task: directory FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint32_t code_phys =
                frame_alloc();

            if (code_phys == FRAME_INVALID ||
                code_phys >= PAGING_IDENTITY_LIMIT) {

                if (code_phys != FRAME_INVALID)
                    frame_free(code_phys);

                frame_free(directory);
                console_error("Ring 3 task: code frame FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint8_t *code_dst =
                (uint8_t *)code_phys;

            for (uint32_t k = 0;
                 k < ring3_test_program_size;
                 k++) {

                code_dst[k] =
                    ring3_test_program[k];
            }

            if (paging_map_in_directory(
                    directory,
                    TASK_RING3_CODE_VA,
                    code_phys,
                    PAGE_PRESENT | PAGE_USER
                ) != 0) {

                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: code map FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint32_t stack_phys =
                frame_alloc();

            if (stack_phys == FRAME_INVALID ||
                stack_phys >= PAGING_IDENTITY_LIMIT) {

                if (stack_phys != FRAME_INVALID)
                    frame_free(stack_phys);

                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: stack frame FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            uint8_t *stack_dst =
                (uint8_t *)stack_phys;

            for (uint32_t k = 0;
                 k < PAGE_SIZE;
                 k++) {

                stack_dst[k] = 0;
            }

            if (paging_map_in_directory(
                    directory,
                    TASK_RING3_STACK_VA,
                    stack_phys,
                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER
                ) != 0) {

                frame_free(stack_phys);
                frame_free(code_phys);
                frame_free(directory);
                console_error("Ring 3 task: stack map FAILED");
                tasks[i].state = TASK_UNUSED;
                __asm__ volatile ("sti");
                return -1;
            }

            tasks[i].page_directory = directory;

            for (int j = 0; j < MAX_FDS; j++) {
                tasks[i].fd_table.open[j] = 0;
            }
            tasks[i].cwd_inode = tasks[0].cwd_inode;

            tasks[i].switch_esp =
                task_prepare_user_stack(
                    tasks[i].stack_base,
                    tasks[i].stack_size,
                    TASK_RING3_CODE_VA,
                    TASK_RING3_STACK_TOP
                );

            __asm__ volatile ("sti");
        }

        active_tasks++;

        return (int)i;
    }

    return -1;
}

int task_create(void)
{
    return task_create_with_privilege(KERNEL_RING);
}

int task_create_user_program(const uint8_t *code, uint32_t code_size)
{
    return task_create_ring3_with_code(code, code_size);
}

/* Create a Ring 3 task from an ELF executable image.
 * This parses the ELF headers and maps each PT_LOAD segment
 * into the task's address space with appropriate permissions. */
int task_create_user_elf(const Elf32_Ehdr *ehdr, const void *buffer, uint32_t size)
{
    if (size < sizeof(Elf32_Ehdr))
        return -1;

    /* Verify ELF magic. */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_type != ET_EXEC ||
        ehdr->e_machine != EM_386) {
        return -1;
    }

    uint32_t i;

    for (i = 1; i <= MAX_TASKS; i++) {

        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_TERMINATED)
            break;
    }

    if (i > MAX_TASKS)
        return -1;

    tasks[i].id = i;
    tasks[i].parent_id = scheduler_current_task();
    tasks[i].state = TASK_READY;
    tasks[i].privilege = USER_RING;

    tasks[i].stack_base = (uint32_t)&task_stacks[i][0];
    tasks[i].stack_size = TASK_STACK_SIZE;

    tasks[i].context.eax = 0;
    tasks[i].context.ebx = 0;
    tasks[i].context.ecx = 0;
    tasks[i].context.edx = 0;
    tasks[i].context.esi = 0;
    tasks[i].context.edi = 0;
    tasks[i].context.eflags = 0x202;

    __asm__ volatile ("cli");

    uint32_t directory = paging_create_address_space();

    if (directory == FRAME_INVALID) {
        tasks[i].state = TASK_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

/* Parse program headers and map each PT_LOAD segment. */
    const Elf32_Phdr *phdr = (const Elf32_Phdr *)((const uint8_t *)ehdr + ehdr->e_phoff);

    /* Track physical pages allocated for this task's segments.
     * We need to track them because paging_virtual_to_physical only works
     * with the current CR3, not an arbitrary directory. */
    uint32_t segment_phys[16];  /* Max 16 pages per segment */
    uint32_t segment_vaddr[16];
    uint32_t segment_pages = 0;

    for (uint16_t j = 0; j < ehdr->e_phnum; j++) {
        const Elf32_Phdr *ph = &phdr[j];

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0)
            continue;

        uint32_t vaddr = ph->p_vaddr;
        uint32_t memsz = ph->p_memsz;
        uint32_t seg_flags = ph->p_flags;

        /* Align to page boundary. */
        uint32_t vaddr_aligned = vaddr & ~0xFFF;
        uint32_t end_addr = (vaddr + memsz + 0xFFF) & ~0xFFF;
        uint32_t num_pages = (end_addr - vaddr_aligned) / PAGE_SIZE;

        if (vaddr_aligned < TASK_RING3_CODE_VA || vaddr_aligned >= 0xC0000000) {
            console_error("Loader: invalid virtual address");
            return -1;
        }

        /* Allocate and map pages. Track physical pages for data copy. */
        for (uint32_t p = 0; p < num_pages; p++) {
            uint32_t page_vaddr = vaddr_aligned + p * PAGE_SIZE;
            uint32_t page_phys = frame_alloc();

            if (page_phys == FRAME_INVALID || page_phys >= PAGING_IDENTITY_LIMIT) {
                /* Cleanup on failure. */
                for (uint32_t k = 0; k < p; k++)
                    frame_free(vaddr_aligned + k * PAGE_SIZE);
                return -1;
            }

            uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (seg_flags & PF_W)
                page_flags |= PAGE_WRITE;

            if (paging_map_in_directory(directory, page_vaddr, page_phys, page_flags) != 0) {
                for (uint32_t k = 0; k < p; k++)
                    frame_free(vaddr_aligned + k * PAGE_SIZE);
                return -1;
            }

            /* Track physical page for data copy. */
            if (segment_pages < 16) {
                segment_phys[segment_pages] = page_phys;
                segment_vaddr[segment_pages] = page_vaddr;
                segment_pages++;
            }
        }

        /* Copy file data into mapped pages using tracked physical pages. */
        if (ph->p_filesz > 0) {
            const uint8_t *src = (const uint8_t *)((const uint8_t *)buffer + ph->p_offset);
            uint32_t remaining = ph->p_filesz;
            uint32_t pos = 0;

            while (remaining > 0) {
                uint32_t page_offset = (ph->p_vaddr + pos) & 0xFFF;
                uint32_t page_vaddr = ph->p_vaddr + pos;

                /* Find the physical page for this virtual address. */
                uint32_t page_phys = 0;
                for (uint32_t p = 0; p < segment_pages; p++) {
                    uint32_t seg_start = segment_vaddr[p];
                    uint32_t seg_end = seg_start + PAGE_SIZE;
                    if (page_vaddr >= seg_start && page_vaddr < seg_end) {
                        page_phys = segment_phys[p];
                        break;
                    }
                }

                if (page_phys == 0) {
                    console_error("Loader: page not mapped");
                    return -1;
                }

                uint32_t chunk = PAGE_SIZE - page_offset;
                if (chunk > remaining)
                    chunk = remaining;

                /* Copy to physical page. */
                uint8_t *dst = (uint8_t *)page_phys + page_offset;
                for (uint32_t k = 0; k < chunk; k++)
                    dst[k] = src[k];

                pos += chunk;
                remaining -= chunk;
            }
        }
    }

    /* Map stack. */
    uint32_t stack_phys = frame_alloc();
    if (stack_phys == FRAME_INVALID || stack_phys >= PAGING_IDENTITY_LIMIT) {
        return -1;
    }

    /*
     * Map the stack at TASK_RING3_STACK_VA. It used to go at
     * TASK_RING3_STACK_VA - PAGE_SIZE, which is 0x01000000 -- the
     * very address the ELF's code segment is mapped at. The stack
     * therefore replaced the loaded code, and the task resumed into a
     * page of zeros.
     */
    if (paging_map_in_directory(directory, TASK_RING3_STACK_VA, stack_phys,
            PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        frame_free(stack_phys);
        return -1;
    }

    /* Zero the stack page. */
    uint8_t *stack_dst = (uint8_t *)stack_phys;
    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        stack_dst[k] = 0;

    /* Set up the task. */
    tasks[i].page_directory = directory;

    for (int j = 0; j < MAX_FDS; j++) {
        tasks[i].fd_table.open[j] = 0;
    }
    tasks[i].cwd_inode = tasks[0].cwd_inode;

    tasks[i].signals.pending = 0;
    tasks[i].signals.mask = 0;
    for (int s = 0; s < 32; s++) {
        tasks[i].signals.handlers[s] = 0;
    }

    tasks[i].context.eip = ehdr->e_entry;
    tasks[i].context.esp = TASK_RING3_STACK_TOP;

    /* Enter at the program's own entry point. Hardcoding
     * TASK_RING3_CODE_VA ignores e_entry, which is wrong for any
     * image not linked at the start of its text segment. */
    tasks[i].switch_esp =
        task_prepare_user_stack(
            tasks[i].stack_base,
            tasks[i].stack_size,
            ehdr->e_entry,
            TASK_RING3_STACK_TOP
        );

    __asm__ volatile ("sti");

    active_tasks++;

    return (int)i;
}

/*
 * Fork for a Ring 3 caller.
 *
 * 'frame' is the interrupted context the syscall stub pushed, which is
 * where the child has to resume: immediately after the `int 0x80`
 * that performed the fork, with EAX already zeroed so the child sees
 * fork() return 0. Building that frame on the child's own kernel stack
 * is what lets both tasks continue from the same user instruction
 * with independent return values.
 *
 * The child gets a private copy of the parent's user pages, a copy of
 * the parent's live stack contents, and copies of its file
 * descriptors, working directory and signal state.
 */
int task_fork_user(uint32_t *frame)
{
    if (frame == 0)
        return -1;

    uint32_t parent_id = scheduler_current_task();
    task_t *parent = (task_t *)task_get(parent_id);

    if (parent == 0 || parent->privilege != USER_RING)
        return -1;

    int child_id = -1;

    for (uint32_t i = 1; i <= MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED ||
            tasks[i].state == TASK_TERMINATED) {
            child_id = (int)i;
            break;
        }
    }

    if (child_id < 0)
        return -1;

    __asm__ volatile ("cli");

    uint32_t child_dir = 0;

    if (paging_clone_address_space(parent->page_directory,
                                   &child_dir) != 0) {
        __asm__ volatile ("sti");
        return -1;
    }

    task_t *child = &tasks[child_id];

    child->id = (uint32_t)child_id;
    child->parent_id = parent_id;
    child->state = TASK_READY;
    child->privilege = USER_RING;
    child->page_directory = child_dir;
    child->stack_base = (uint32_t)&task_stacks[child_id][0];
    child->stack_size = TASK_STACK_SIZE;
    child->cwd_inode = parent->cwd_inode;
    child->signals = parent->signals;

    for (int j = 0; j < MAX_FDS; j++) {
        child->fd_table.open[j] = parent->fd_table.open[j];

        if (parent->fd_table.open[j])
            child->fd_table.handles[j] = parent->fd_table.handles[j];
    }

    /*
     * Copy the interrupted context onto the child's kernel stack and
     * point switch_esp at it. The stub pops the pusha block and then
     * iret's, so the whole frame has to be reproduced verbatim --
     * EIP, CS, EFLAGS, SS and the user ESP included -- or the child
     * would resume in Ring 0 with the parent's registers.
     *
     * task_stacks[] is static kernel memory, so this is a plain copy
     * from the current context: no need to migrate the CPU onto the
     * child's stack to write it.
     */
    uint32_t child_stack_top = child->stack_base + child->stack_size;
    uint32_t child_frame = child_stack_top - 128;
    uint32_t *dst = (uint32_t *)child_frame;

    for (uint32_t w = 0; w < 15; w++)
        dst[w] = frame[w];

    /* The child resumes here with EAX = 0. */
    dst[7] = 0;

    /* Keep the saved context struct in step, since save_task_context()
     * reads back from this same frame on the next switch. */
    child->context.edi = dst[0];
    child->context.esi = dst[1];
    child->context.ebp = dst[2];
    child->context.esp = (uint32_t)dst + 32;
    child->context.ebx = dst[4];
    child->context.edx = dst[5];
    child->context.ecx = dst[6];
    child->context.eax = 0;
    child->context.eip = dst[8];
    child->context.eflags = dst[10];

    child->switch_esp = child_frame;

    active_tasks++;

    __asm__ volatile ("sti");

    return child_id;
}

int task_fork(void)
{
    uint32_t parent_id = scheduler_current_task();
    const task_t *parent = task_get(parent_id);
    if (parent == 0)
        return -1;

    if (parent->privilege == USER_RING) {
        return -1;
    }

    int child_id = task_create();
    if (child_id < 0)
        return -1;

    task_t *child = (task_t *)task_get(child_id);
    if (child == 0) {
        task_terminate(child_id);
        return -1;
    }

    child->parent_id = parent_id;
    child->context.eax = 0;
    child->context.ebx = parent->context.ebx;
    child->context.ecx = parent->context.ecx;
    child->context.edx = parent->context.edx;
    child->context.esi = parent->context.esi;
    child->context.edi = parent->context.edi;
    child->context.ebp = parent->context.ebp;
    child->context.esp = parent->context.esp;
    child->context.eip = parent->context.eip;
    child->context.eflags = parent->context.eflags;

    for (int j = 0; j < MAX_FDS; j++) {
        child->fd_table.open[j] = parent->fd_table.open[j];
        if (parent->fd_table.open[j]) {
            child->fd_table.handles[j] = parent->fd_table.handles[j];
        }
    }
    child->cwd_inode = parent->cwd_inode;

    child->signals.pending = parent->signals.pending;
    child->signals.mask = parent->signals.mask;
    for (int s = 0; s < 32; s++) {
        child->signals.handlers[s] = parent->signals.handlers[s];
    }

    return child_id;
}

/*
 * exec() for a Ring 3 task: replace this task's image with the
 * contents of a file on LumenFS and continue at Ring 3 in it.
 *
 * The new image is entered by rewriting the live syscall frame so the
 * stub's popa/iret lands in it. The previous version built a fresh
 * user stack into task->switch_esp instead, but the very next context
 * save overwrote switch_esp with the still-current syscall frame --
 * so the task carried on executing the old program and the exec'd one
 * never ran.
 *
 * The outgoing address space is not torn down here: the CPU is still
 * running from it until the frame is popped. It is recorded for
 * task_wait() to release, the same way a normal exit defers.
 *
 * Returns 1 on success, 0 on failure.
 */
int sys_exec(const char *filename, uint32_t *frame)
{
    if (filename == 0 || frame == 0)
        return 0;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return 0;

    if (task->privilege != USER_RING)
        return 0;

    fs_handle_t *fd = kmalloc(sizeof(fs_handle_t));

    if (fd == 0)
        return 0;

    /* flags == 0 is a read-only open. */
    if (fs_open(filename, FS_ROOT, 0, fd) != FS_OK) {
        kfree(fd);
        return 0;
    }

    uint32_t file_size = 0;

    if (fs_handle_size(fd, &file_size) != FS_OK ||
        file_size == 0 ||
        file_size > PAGE_SIZE) {
        fs_close(fd);
        kfree(fd);
        return 0;
    }

    uint8_t *code = kmalloc(file_size);

    if (code == 0) {
        fs_close(fd);
        kfree(fd);
        return 0;
    }

    uint32_t read = 0;

    if (fs_handle_read(fd, code, file_size, &read) != FS_OK ||
        read != file_size) {
        kfree(code);
        fs_close(fd);
        kfree(fd);
        return 0;
    }

    fs_close(fd);
    kfree(fd);

    uint32_t directory = paging_create_address_space();

    if (directory == FRAME_INVALID)
        return 0;

    uint32_t code_phys = frame_alloc();

    if (code_phys == FRAME_INVALID ||
        code_phys >= PAGING_IDENTITY_LIMIT) {
        if (code_phys != FRAME_INVALID)
            frame_free(code_phys);

        paging_destroy_address_space(directory);
        return 0;
    }

    uint8_t *code_dst = (uint8_t *)code_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        code_dst[k] = (k < file_size) ? code[k] : 0;

    if (paging_map_in_directory(directory, TASK_RING3_CODE_VA, code_phys,
                                PAGE_PRESENT | PAGE_USER) != 0) {
        frame_free(code_phys);
        paging_destroy_address_space(directory);
        return 0;
    }

    uint32_t stack_phys = frame_alloc();

    if (stack_phys == FRAME_INVALID ||
        stack_phys >= PAGING_IDENTITY_LIMIT) {
        if (stack_phys != FRAME_INVALID)
            frame_free(stack_phys);

        frame_free(code_phys);
        paging_destroy_address_space(directory);
        return 0;
    }

    uint8_t *stack_dst = (uint8_t *)stack_phys;

    for (uint32_t k = 0; k < PAGE_SIZE; k++)
        stack_dst[k] = 0;

    if (paging_map_in_directory(directory, TASK_RING3_STACK_VA, stack_phys,
                                PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        frame_free(stack_phys);
        frame_free(code_phys);
        paging_destroy_address_space(directory);
        return 0;
    }

    /*
     * The old image is still in use until the frame is popped, so hand
     * it to task_wait() rather than freeing it now.
     *
     * CR3 must be pointed at the new directory *before* returning.
     * Nothing else reloads it until this task is next scheduled, and
     * the iret on the way out would otherwise land back in the old
     * code page -- which re-entered the old program instead of the
     * new one. Both directories share the master's kernel mappings, so
     * switching while still executing on the kernel stack is safe.
     */
    task->reclaim_directory = task->page_directory;
    task->page_directory = directory;

    paging_switch_directory(directory);

    task->signals.pending = 0;
    task->signals.mask = 0;

    for (int s = 0; s < 32; s++)
        task->signals.handlers[s] = 0;

    /*
     * Point the interrupted context at the new image: popa will load
     * EAX 0, and iret will enter the new program at CPL 3 with a fresh
     * user stack.
     *
     * On a privilege change the CPU pushed, lowest address first,
     * EIP, CS, EFLAGS, ESP, SS -- so ESP comes before SS here. The two
     * were the wrong way round, and iret loaded SS with a stack
     * pointer, which raised a #GP.
     */
    frame[7] = 0;
    frame[8] = TASK_RING3_CODE_VA;
    frame[9] = USER_CODE_SELECTOR;
    frame[10] = 0x00000202;
    frame[11] = TASK_RING3_STACK_TOP;
    frame[12] = USER_DATA_SELECTOR;

    return 1;
}

int task_terminate(uint32_t id)
{
    if (id == 0 || id > MAX_TASKS)
        return -1;

    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return -1;

    if (task->state == TASK_UNUSED ||
        task->state == TASK_TERMINATED)
        return -1;

    /*
     * Do NOT destroy the Ring 3 address space here.
     *
     * SYS_EXIT is executed while the CPU is still using the
     * process address space. Destroying it before the syscall
     * return path switches tasks would make iret return into
     * unmapped user code.
     *
     * Resources are reclaimed by task_wait().
     */
    task->state = TASK_TERMINATED;

    if (active_tasks > 0)
        active_tasks--;

    return 0;
}

int task_block(uint32_t id)
{
    if (id == 0 || id > MAX_TASKS)
        return -1;

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_READY &&
        task->state != TASK_RUNNING)
        return -1;

    task->state = TASK_BLOCKED;
    return 0;
}

int task_wake(uint32_t id)
{
    if (id > MAX_TASKS)
        return -1;

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_BLOCKED)
        return -1;

    task->state = TASK_READY;
    return 0;
}


int task_yield(void)
{
    uint32_t id = scheduler_current_task();

    task_t *task = (task_t *)task_get(id);

    if (task == 0)
        return -1;

    if (task->state != TASK_RUNNING)
        return -1;

    /*
     * The next timer interrupt performs the actual switch.
     */
    task->state = TASK_READY;

    return 0;
}

int task_exit(void)
{
    uint32_t id = scheduler_current_task();

    if (id == 0)
        return -1;

    /*
     * Only mark the task terminated here.
     *
     * The syscall assembly path immediately switches away from
     * this task, so its address space remains valid until the
     * parent calls task_wait().
     */
    return task_terminate(id);
}

int task_wait(uint32_t child_id)
{
    uint32_t parent_id = scheduler_current_task();

    if (parent_id > MAX_TASKS)
        return -1;

    if (child_id == 0 || child_id > MAX_TASKS)
        return -1;

    if (child_id == parent_id)
        return -1;

    task_t *child = (task_t *)task_get(child_id);
    if (child == 0)
        return -1;

    /*
     * Only a terminated child can be reaped.
     */
    if (child->parent_id != parent_id)
        return -1;

    if (child->state != TASK_TERMINATED)
        return -1;

    /*
     * The child is no longer executing, so its Ring 3 address
     * space can safely be destroyed now.
     */
    if (child->privilege == USER_RING &&
        child->page_directory != PAGE_DIRECTORY_ADDRESS) {

        __asm__ volatile ("cli");

        if (paging_current_directory() == child->page_directory)
            paging_switch_directory(PAGE_DIRECTORY_ADDRESS);

        paging_destroy_address_space(child->page_directory);

        child->page_directory = PAGE_DIRECTORY_ADDRESS;

        /* An image retired by exec() is still outstanding. */
        if (child->reclaim_directory != 0 &&
            child->reclaim_directory != PAGE_DIRECTORY_ADDRESS) {

            paging_destroy_address_space(child->reclaim_directory);
        }

        __asm__ volatile ("sti");
    }

    child->reclaim_directory = 0;

    /*
     * Close any open file descriptors.
     */
    for (int j = 0; j < MAX_FDS; j++) {
        if (child->fd_table.open[j]) {
            fs_close(&child->fd_table.handles[j]);
            child->fd_table.open[j] = 0;
        }
    }

    /*
     * Send SIGCHLD to parent.
     */
    task_t *parent = (task_t *)task_get(parent_id);
    if (parent != 0) {
        parent->signals.pending |= (1u << SIGCHLD);
    }

    /*
     * Reclaim the process table slot.
     */
    child->id = 0;
    child->parent_id = 0;
    child->state = TASK_UNUSED;
    child->privilege = KERNEL_RING;
    child->page_directory = PAGE_DIRECTORY_ADDRESS;
    child->stack_base = 0;
    child->stack_size = 0;
    child->switch_esp = 0;
    child->reclaim_directory = 0;

    child->context.eax = 0;
    child->context.ebx = 0;
    child->context.ecx = 0;
    child->context.edx = 0;
    child->context.esi = 0;
    child->context.edi = 0;
    child->context.ebp = 0;
    child->context.esp = 0;
    child->context.eip = 0;
    child->context.eflags = 0x202;

    return (int)child_id;
}

/*
 * Signal handling functions.
 */

int sys_kill(uint32_t pid, int sig)
{
    if (sig < 1 || sig >= 32)
        return 0;

    if (pid == 0 || pid > MAX_TASKS)
        return 0;

    task_t *target = (task_t *)task_get(pid);
    if (target == 0)
        return 0;

    if (target->state == TASK_UNUSED || target->state == TASK_TERMINATED)
        return 0;

    target->signals.pending |= (1u << sig);
    return 1;
}

int sys_signal(int sig, void (*handler)(int))
{
    if (sig < 1 || sig >= 32)
        return 0;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return 0;

    task->signals.handlers[sig] = handler;
    return 1;
}

int sys_sigprocmask(int how, uint32_t mask)
{
    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return 0xFFFFFFFFU;

    uint32_t old_mask = task->signals.mask;

    switch (how) {
        case 0:  /* SIG_BLOCK */
            task->signals.mask |= mask;
            break;
        case 1:  /* SIG_UNBLOCK */
            task->signals.mask &= ~mask;
            break;
        case 2:  /* SIG_SETMASK */
            task->signals.mask = mask;
            break;
        default:
            return 0xFFFFFFFFU;
    }

    return old_mask;
}

/*
 * File descriptor and pipe syscalls.
 */

int sys_pipe(int *fds)
{
    int pipe_fd = pipe_create();
    if (pipe_fd < 0)
        return 0;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return 0;

    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!task->fd_table.open[i]) {
            if (read_fd == -1)
                read_fd = i;
            else if (write_fd == -1) {
                write_fd = i;
                break;
            }
        }
    }

    if (read_fd == -1 || write_fd == -1) {
        /* Close the pipe if we can't allocate fds */
        pipe_close_read(pipe_fd);
        pipe_close_write(pipe_fd);
        return 0;
    }

    task->fd_table.handles[read_fd].inode = pipe_fd;
    task->fd_table.open[read_fd] = 1;
    task->fd_table.handles[write_fd].inode = pipe_fd;
    task->fd_table.open[write_fd] = 1;

    fds[0] = read_fd;
    fds[1] = write_fd;

    return 1;
}

int sys_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return 0;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(scheduler_current_task());
    if (task == 0)
        return 0;

    if (task->fd_table.open[oldfd] == 0)
        return 0;

    if (oldfd == newfd)
        return 1;

    if (task->fd_table.open[newfd]) {
        /* Close the newfd first */
        fs_close(&task->fd_table.handles[newfd]);
        task->fd_table.open[newfd] = 0;
    }

    task->fd_table.handles[newfd] = task->fd_table.handles[oldfd];
    task->fd_table.open[newfd] = 1;

    return 1;
}

int sys_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return 0;

    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (task == 0)
        return 0;

    if (task->fd_table.open[fd] == 0)
        return 0;

    /* For pipes, close the appropriate end */
    uint32_t pipe_fd = task->fd_table.handles[fd].inode;
    if (pipe_fd < MAX_PIPES) {
        /* Determine if this is read or write end by checking the pipe's readers/writers */
        /* For simplicity, we'll just close both ends */
        pipe_close_read(pipe_fd);
        pipe_close_write(pipe_fd);
    } else {
        fs_close(&task->fd_table.handles[fd]);
    }

    task->fd_table.open[fd] = 0;
    return 1;
}


int sys_open(const char *path, uint32_t flags)
{
    if (!path) return -1;
    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (!task) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_FDS; i++) if (!task->fd_table.open[i]) { slot = i; break; }
    if (slot < 0) return -1;
    if (fs_open(path, FS_ROOT, flags, &task->fd_table.handles[slot]) != FS_OK) return -1;
    task->fd_table.open[slot] = 1;
    return slot;
}
int sys_read(int fd, void *buf, uint32_t len)
{
    if (fd < 0 || fd >= MAX_FDS || !buf) return -1;
    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (!task || !task->fd_table.open[fd]) return -1;
    uint32_t n=0; return fs_handle_read(&task->fd_table.handles[fd], buf, len, &n)==FS_OK?(int)n:-1;
}
int sys_write(int fd, const void *buf, uint32_t len)
{
    if (fd < 0 || fd >= MAX_FDS || !buf) return -1;
    uint32_t id = scheduler_current_task();
    task_t *task = (task_t *)task_get(id);
    if (!task || !task->fd_table.open[fd]) return -1;
    uint32_t n=0; return fs_handle_write(&task->fd_table.handles[fd], buf, len, &n)==FS_OK?(int)n:-1;
}

const task_t *task_get(uint32_t id)
{
    if (id > MAX_TASKS)
        return 0;

    if (tasks[id].state == TASK_UNUSED)
        return 0;

    return &tasks[id];
}

uint32_t task_count(void)
{
    return active_tasks;
}

int task_run_self_test(void)
{
    uint32_t before = active_tasks;

    int first = task_create();
    if (first < 0)
        return 0;

    int second = task_create();
    if (second < 0) {
        task_terminate((uint32_t)first);
        return 0;
    }

    const task_t *a = task_get((uint32_t)first);
    const task_t *b = task_get((uint32_t)second);

    if (a == 0 || b == 0)
        return 0;

    if (a->stack_base == b->stack_base)
        return 0;

    if (a->stack_size != TASK_STACK_SIZE ||
        b->stack_size != TASK_STACK_SIZE)
        return 0;

    if (a->state != TASK_READY ||
        b->state != TASK_READY)
        return 0;

    if (a->switch_esp == 0 ||
        b->switch_esp == 0)
        return 0;

    if (task_block((uint32_t)first) != 0)
        return 0;

    if (a->state != TASK_BLOCKED)
        return 0;

    if (task_wake((uint32_t)first) != 0)
        return 0;

    if (a->state != TASK_READY)
        return 0;

    if (task_terminate((uint32_t)first) != 0)
        return 0;

    if (task_terminate((uint32_t)second) != 0)
        return 0;

    if (active_tasks != before)
        return 0;

    return 1;
}
