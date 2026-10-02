; Ring 3 file-I/O self-test.
;
; This is ordinary user-mode code: it runs at CPL 3 and is the only
; thing in the suite that pushes the whole stack together --
; paging, uaccess pointer validation, the fd table, and LumenFS --
; in a single end-to-end exercise. Everything it learns is recorded
; into fixed user addresses, which the kernel reads back once the
; task has terminated.
;
; Syscall convention (see kernel/syscall.c):
;     eax = number, ebx = arg1, edx = arg2, ecx = arg3
;     result comes back in eax; errors come back as -1.

BITS 32

; The Ring 3 code page is mapped read-only; see ring3.h.
TASK_RING3_CODE_VA equ 0x01000000

global ring3_io_entry
global ring3_io_size
global ring3_fault_entry
global ring3_badop_entry
global ring3_fault_size
global ring3_badop_size
global ring3_fork_entry
global ring3_fork_size
global ring3_exec_entry
global ring3_execprog
global ring3_exec_size
global ring3_execprog_len
global ring3_syscalls_entry
global ring3_syscalls_size

; User addresses. These sit at the very bottom of the stack page
; (0x01001000), well below where the stack pointer starts
; (0x01002000), so a shallow program cannot collide with them.
PATH_ADDR     equ 0x01001000
DATA_ADDR     equ 0x01001040
RES_BASE      equ 0x010010C0

RES_OPEN1     equ RES_BASE + 0x00   ; first open, expect >= 0
RES_WRITE     equ RES_BASE + 0x04   ; bytes written, expect DATA_LEN
RES_CLOSE1    equ RES_BASE + 0x08   ; close, expect 0
RES_OPEN2     equ RES_BASE + 0x0C   ; reopen, expect >= 0
RES_READ      equ RES_BASE + 0x10   ; bytes read, expect DATA_LEN
RES_CMP       equ RES_BASE + 0x14   ; data compared equal, expect 1
RES_BADWRITE  equ RES_BASE + 0x18   ; kernel-pointer write, expect -1
RES_BADREAD   equ RES_BASE + 0x1C   ; kernel-pointer read, expect -1
RES_BADOPEN   equ RES_BASE + 0x20   ; kernel-pointer path, expect -1
RES_GETPID    equ RES_BASE + 0x24   ; own pid, expect >= 1
RES_YIELD     equ RES_BASE + 0x28   ; yield, expect 0
RES_CLOSE2    equ RES_BASE + 0x2C   ; close again, expect 0
RES_REOPEN_RD equ RES_BASE + 0x30   ; read-only open for a 2nd pass
RES_SHORTREAD equ RES_BASE + 0x34   ; read past EOF, expect 0

DATA_LEN      equ 8

; Flags, matching fs.h.
O_RDONLY      equ 0x00
O_RDWR        equ 0x02
O_CREAT       equ 0x04
O_TRUNC       equ 0x08

; A kernel address: far above USER_ADDR_MAX, so uaccess must reject
; any attempt to pass it.
KERNEL_BAD    equ 0x00100000

SYS_EXIT      equ 3
SYS_GETPID    equ 1
SYS_YIELD     equ 2
SYS_CLOSE     equ 17
SYS_OPEN      equ 18
SYS_READ      equ 19
SYS_WRITE     equ 20

; call_sys: eax=number, ebx=arg1, edx=arg2, ecx=arg3 -> eax=result
%macro call_sys 4
    mov eax, %1
    mov ebx, %2
    mov edx, %3
    mov ecx, %4
    int 0x80
%endmacro

; Store eax at a fixed user address (absolute moffs form).
%macro store_res 1
    mov dword [%1], eax
%endmacro

section .text
ring3_io_entry:
    ; ---- the file name, and the payload -------------------------
    ; "/tmp/io.txt", built a word at a time (little-endian).
    mov dword [PATH_ADDR],     0x706D742F     ; "/tmp"
    mov dword [PATH_ADDR + 4], 0x2E6F692F     ; "/io."
    mov dword [PATH_ADDR + 8], 0x00747874     ; "txt\0"

    mov dword [DATA_ADDR],     0x44434241     ; "ABCD"
    mov dword [DATA_ADDR + 4], 0x48474645     ; "EFGH"

    ; ---- 1. create + write --------------------------------------
    call_sys SYS_OPEN, PATH_ADDR, O_RDWR | O_CREAT | O_TRUNC, 0
    store_res RES_OPEN1

    cmp eax, 0
    jl .fail

    mov esi, eax                   ; keep the fd in esi

    call_sys SYS_WRITE, esi, DATA_ADDR, DATA_LEN
    store_res RES_WRITE

    call_sys SYS_CLOSE, esi, 0, 0
    store_res RES_CLOSE1

    ; ---- 2. reopen and read it back ------------------------------
    call_sys SYS_OPEN, PATH_ADDR, O_RDONLY, 0
    store_res RES_OPEN2

    cmp eax, 0
    jl .fail

    mov edi, eax

    ; Overwrite the buffer with a pattern that cannot match, so a
    ; read that writes nothing at all still fails the comparison.
    mov dword [DATA_ADDR], 0xFFFFFFFF

    call_sys SYS_READ, edi, DATA_ADDR, DATA_LEN
    store_res RES_READ

    ; Compare the first 4 bytes against "ABCD".
    mov eax, [DATA_ADDR]
    cmp eax, 0x44434241
    jne .cmp_bad
    mov eax, [DATA_ADDR + 4]
    cmp eax, 0x48474645
    jne .cmp_bad

    mov dword [RES_CMP], 1
    jmp .after_cmp

.cmp_bad:
    mov dword [RES_CMP], 0

.after_cmp:

    ; A read at EOF must report 0, not stale data or an error.
    call_sys SYS_READ, edi, DATA_ADDR, DATA_LEN
    store_res RES_SHORTREAD

    call_sys SYS_CLOSE, edi, 0, 0
    store_res RES_CLOSE2

    ; ---- 3. the kernel must refuse kernel pointers ---------------
    call_sys SYS_WRITE, 0, KERNEL_BAD, 4
    store_res RES_BADWRITE

    call_sys SYS_READ, 0, KERNEL_BAD, 4
    store_res RES_BADREAD

    call_sys SYS_OPEN, KERNEL_BAD, O_RDONLY, 0
    store_res RES_BADOPEN

    ; ---- 4. the cheap syscalls -----------------------------------
    call_sys SYS_GETPID, 0, 0, 0
    store_res RES_GETPID

    call_sys SYS_YIELD, 0, 0, 0
    store_res RES_YIELD

    ; ---- 5. hand over a known exit status ------------------------
    call_sys SYS_EXIT, 0, 0, 0
    jmp .fail_hang

.fail:
    call_sys SYS_EXIT, 1, 0, 0

.fail_hang:
    ; If exit ever stops working the scheduler would run us off the
    ; end, so park here instead. The kernel times the wait out.
    jmp .fail_hang

ring3_io_entry_end:

; ---- Fault probes -----------------------------------------------------
;
; Both of these deliberately fault. A fault taken in Ring 3 belongs to
; the offending program: the kernel must retire just that task and keep
; running. If either one halted the machine, every self-test after it
; would stop being reported -- which is exactly what the harness checks.

; Read from an unmapped user address: page fault (vector 14).
ring3_fault_entry:
    mov eax, 0x40000000        ; unmapped, well inside the user window
    mov ebx, [eax]             ; faults here
    mov dword [RES_BASE], ebx  ; only reached if nothing faulted
    jmp ring3_fault_entry

ring3_fault_entry_end:

; Execute an undefined instruction: invalid opcode (vector 6). The
; first probe already covers #PF, and #PF alone would not show that the
; recoverable-vector list is wired up beyond a single entry.
;
; (Writing to the read-only code page was the obvious second case, but
; paging -- not segment protection -- is what enforces it here, so it
; raised a second #PF rather than #GP.)
ring3_badop_entry:
    ud2                                     ; faults here
    jmp ring3_badop_entry

ring3_badop_entry_end:


; The kernel passes ring3_io_size as the code size, which is exactly
; the number of bytes to copy into the Ring 3 page. It has to live in
; a section with real storage: as an `equ` it is an absolute symbol
; with no address, and reading it from C yields garbage -- which made
; task_create_user_program() reject the code as too large.
section .data
align 4
global ring3_io_size
ring3_io_size:
    dd ring3_io_entry_end - ring3_io_entry

align 4
global ring3_fault_size
ring3_fault_size:
    dd ring3_fault_entry_end - ring3_fault_entry

align 4
global ring3_badop_size
ring3_badop_size:
    dd ring3_badop_entry_end - ring3_badop_entry
; ---- Fork probe -------------------------------------------------------
;
; The parent forks; the child must see fork() return 0 and the parent a
; positive child id. Both then exit, and the kernel reaps them and
; checks the parent/child relationship held while they ran.

RES_FORK_PID  equ RES_BASE + 0x40     ; parent: the child id, expect > 0
RES_FORK_CHILD equ RES_BASE + 0x44    ; child: 0
RES_FORK_PPID  equ RES_BASE + 0x48    ; child: parent's pid, expect > 0
RES_FORK_PPID2 equ RES_BASE + 0x4C    ; parent: its own parent id
RES_FORK_WAIT  equ RES_BASE + 0x50    ; parent: SYS_WAIT result, expect > 0
RES_FORK_DONE  equ RES_BASE + 0x54    ; child: written last, "results ready"
RES_FORK_PARENTBUF equ RES_BASE + 0x58 ; parent: its own buffer after the child ran
RES_FORK_CHILDBUF  equ RES_BASE + 0x5C ; child: the same address, seen privately
RES_EXEC_RC equ RES_BASE + 0x60    ; launcher: result of SYS_EXEC
RES_FORK_CHILDBUF  equ RES_BASE + 0x5C ; child: the same address, seen privately

; A user page both tasks can reach, seeded before the fork.
BUF_ADDR   equ 0x01001080
PARENT_PAT equ 0xAAAAAAAA
CHILD_PAT  equ 0xBBBBBBBB

FORK_DONE_MAGIC equ 0x00C0FFEE

SYS_GETPPID   equ 7
SYS_EXEC       equ 6
SYS_FORK      equ 5
SYS_WAIT      equ 4

ring3_fork_entry:
    ; Seed a byte pattern before forking. The child will scribble over
    ; this page; if the two tasks really have private address spaces,
    ; the parent's copy must be untouched afterwards.
    mov dword [BUF_ADDR], PARENT_PAT

    call_sys SYS_FORK, 0, 0, 0
    store_res RES_FORK_PID              ; parent: child id; child: 0

    test eax, eax
    jz .child                           ; fork() returned 0: we are the child
    js .failed                          ; negative: fork failed

    ; ---- parent ----
    call_sys SYS_GETPPID, 0, 0, 0
    store_res RES_FORK_PPID2

    ; SYS_WAIT does not block -- it reaps a child that has already
    ; exited -- and task_yield() only marks us runnable, so the real
    ; switch happens on the next 10 ms timer tick. Yielding a fixed few
    ; times therefore does nothing: we would finish the loop long
    ; before a tick. Keep yielding and retrying until the child is
    ; gone, with a bound so a wedged child fails the test instead of
    ; hanging the boot.
    mov esi, 20000

.parent_wait_loop:
    call_sys SYS_YIELD, 0, 0, 0

    mov ebx, [RES_FORK_PID]
    call_sys SYS_WAIT, ebx, 0, 0

    cmp eax, 0
    jge .wait_ok

    dec esi
    jnz .parent_wait_loop

    call_sys SYS_EXIT, 2, 0, 0
    jmp .hang

.wait_ok:
    store_res RES_FORK_WAIT

    ; The child's writes must not have reached this page.
    mov eax, [BUF_ADDR]
    mov dword [RES_FORK_PARENTBUF], eax

    call_sys SYS_EXIT, 0, 0, 0
    jmp .hang

.child:
    ; EAX came back 0, which is exactly what fork() owes the child.
    store_res RES_FORK_CHILD

    ; Overwrite the inherited page, then read it back: a real fork must
    ; show our own write and not the parent's value.
    mov dword [BUF_ADDR], CHILD_PAT
    mov eax, [BUF_ADDR]
    mov dword [RES_FORK_CHILDBUF], eax

    call_sys SYS_GETPPID, 0, 0, 0
    store_res RES_FORK_PPID

    ; Publish the results, then keep running instead of exiting.
    ;
    ; The kernel has to read these out of the child's own address
    ; space, and the parent's SYS_WAIT would reap it -- and with it
    ; release those pages -- before the kernel got a look. The marker
    ; is written last, so its presence means both results are settled.
    mov dword [RES_FORK_DONE], FORK_DONE_MAGIC

.child_spin:
    jmp .child_spin

.failed:
    call_sys SYS_EXIT, 1, 0, 0

.hang:
    jmp .hang

ring3_fork_entry_end:

align 4
global ring3_fork_size
ring3_fork_size:
    dd ring3_fork_entry_end - ring3_fork_entry

; ---- exec test -------------------------------------------------------

section .text

;
; Two pieces. ring3_exec_entry is the launcher: it execs a program
; from the filesystem, which replaces its own image. ring3_execprog
; is the program it loads -- it builds its strings on the stack,
; because the code page is mapped read-only -- and writes a file the
; kernel can check afterwards. That makes the test end to end: exec,
; the replacement image, and file I/O from a freshly loaded task.

EXEC_PATH   equ 0x01001000
EXEC_BUF    equ 0x01001100
EXEC_FLAGS  equ 0x0E          ; O_RDWR | O_CREAT | O_TRUNC

ring3_exec_entry:
    ; Build "/tmp/prog.bin" on the stack page and exec it.
    mov dword [EXEC_PATH],     0x706D742F     ; "/tmp"
    mov dword [EXEC_PATH + 4], 0x6F72702F     ; "/pro"
    mov dword [EXEC_PATH + 8], 0x69622E67     ; "g.bi"
    mov dword [EXEC_PATH + 12], 0x0000006E    ; "n\0"

    call_sys SYS_EXEC, EXEC_PATH, 0, 0
    store_res RES_EXEC_RC

    call_sys SYS_EXIT, 0, 0, 0
    jmp .hang
.hang:
    jmp .hang

ring3_exec_entry_end:

; The program that gets exec'd. Loaded at TASK_RING3_CODE_VA, so it
; must not assume anything about registers on entry and must place
; everything writable on its own stack page.
ring3_execprog:
    ; "/tmp/prog.out"
    mov dword [EXEC_PATH],     0x706D742F     ; "/tmp"
    mov dword [EXEC_PATH + 4], 0x6F72702F     ; "/pro"
    mov dword [EXEC_PATH + 8], 0x756F2E67     ; "g.ou"
    mov dword [EXEC_PATH + 12], 0x00000074    ; "t\0"

    ; "EXEC_OK"
    mov dword [EXEC_BUF],     0x43455845      ; "EXEC"
    mov dword [EXEC_BUF + 4], 0x004B4F5F      ; "_OK\0"

    call_sys SYS_OPEN, EXEC_PATH, EXEC_FLAGS, 0
    test eax, eax
    js .bad

    mov esi, eax
    call_sys SYS_WRITE, esi, EXEC_BUF, 7
    call_sys SYS_CLOSE, esi, 0, 0
    call_sys SYS_EXIT, 0, 0, 0

.bad:
    call_sys SYS_EXIT, 1, 0, 0
    jmp .bad

ring3_execprog_end:

section .data
align 4
global ring3_execprog_len
ring3_execprog_len:
    dd ring3_execprog_end - ring3_execprog

align 4
global ring3_exec_size
ring3_exec_size:
    dd ring3_exec_entry_end - ring3_exec_entry

; ---- syscall coverage ------------------------------------------------
;
; Exercises the syscalls no other probe touches, and -- just as
; importantly -- the error paths, which is where a dispatcher usually
; goes wrong. Every result is recorded for the kernel to check.

RES_SC_UID      equ RES_BASE + 0x70
RES_SC_GID      equ RES_BASE + 0x74
RES_SC_PID      equ RES_BASE + 0x78
RES_SC_PGID     equ RES_BASE + 0x7C
RES_SC_SETSID   equ RES_BASE + 0x80
RES_SC_MASK     equ RES_BASE + 0x84
RES_SC_SIGNAL   equ RES_BASE + 0x88
RES_SC_SIGBAD   equ RES_BASE + 0x8C
RES_SC_SIGRANGE equ RES_BASE + 0x90
RES_SC_DUP2BAD  equ RES_BASE + 0x94
RES_SC_CLOSEBAD equ RES_BASE + 0x98
RES_SC_KILLBAD  equ RES_BASE + 0x9C
RES_SC_UNKNOWN  equ RES_BASE + 0xA0
RES_SC_PID2     equ RES_BASE + 0xA4

SYS_GETUID   equ 8
SYS_GETGID   equ 9
SYS_SETSID   equ 10
SYS_GETPGID  equ 11
SYS_KILL     equ 12
SYS_SIGNAL   equ 13
SYS_SIGMASK  equ 14
SYS_PIPE     equ 15
SYS_DUP2     equ 16
SYS_CLOSE    equ 17

ring3_syscalls_entry:
    call_sys SYS_GETUID, 0, 0, 0
    store_res RES_SC_UID

    call_sys SYS_GETGID, 0, 0, 0
    store_res RES_SC_GID

    call_sys SYS_GETPID, 0, 0, 0
    store_res RES_SC_PID

    call_sys SYS_GETPGID, 0, 0, 0
    store_res RES_SC_PGID

    call_sys SYS_SETSID, 0, 0, 0
    store_res RES_SC_SETSID

    call_sys SYS_SIGMASK, 1, 2, 0            ; SIG_BLOCK, mask bit 1
    store_res RES_SC_MASK

    ; A handler address inside our own user pages is accepted.
    call_sys SYS_SIGNAL, 1, EXEC_BUF, 0
    store_res RES_SC_SIGNAL

    ; A kernel address is not.
    call_sys SYS_SIGNAL, 1, 0x00100000, 0
    store_res RES_SC_SIGBAD

    ; Signal number out of range.
    call_sys SYS_SIGNAL, 99, EXEC_BUF, 0
    store_res RES_SC_SIGRANGE

    call_sys SYS_DUP2, 999, 0, 0
    store_res RES_SC_DUP2BAD

    call_sys SYS_CLOSE, 999, 0, 0
    store_res RES_SC_CLOSEBAD

    call_sys SYS_KILL, 999, 1, 0
    store_res RES_SC_KILLBAD

    ; A number the dispatcher does not know.
    call_sys 99, 0, 0, 0
    store_res RES_SC_UNKNOWN

    ; The task should still be perfectly usable afterwards.
    call_sys SYS_GETPID, 0, 0, 0
    store_res RES_SC_PID2

    call_sys SYS_EXIT, 0, 0, 0
    jmp .hang
.hang:
    jmp .hang

ring3_syscalls_entry_end:

section .data
align 4
global ring3_syscalls_size
ring3_syscalls_size:
    dd ring3_syscalls_entry_end - ring3_syscalls_entry
