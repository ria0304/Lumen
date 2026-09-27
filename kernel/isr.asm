BITS 32

global idt_load

global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31

global isr_default
global isr_syscall

global irq0
global irq1
global irq2
global irq3
global irq4
global irq5
global irq6
global irq7
global irq8
global irq9
global irq10
global irq11
global irq12
global irq13
global irq14
global irq15

extern exception_handler
extern timer_handler
extern kbd_handler
extern irq_unhandled_handler
extern syscall_handler
extern scheduler_irq
extern scheduler_exit_current

idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret


; ------------------------------------------------
; CPU exceptions without an automatic error code
; ------------------------------------------------

%macro ISR_NOERR 1
isr%1:
    pusha

    mov eax, esp
    add eax, 32          ; eax -> EIP (no error code was pushed)
    push eax              ; 3rd arg: frame
    push dword 0            ; 2nd arg: error_code
    push dword %1             ; 1st arg: vector
    call exception_handler
    add esp, 12

    popa
    iret
%endmacro


; ------------------------------------------------
; CPU exceptions with an automatic error code
; ------------------------------------------------

%macro ISR_ERR 1
isr%1:
    pusha

    ; After PUSHA, the CPU error code is 32 bytes
    ; above ESP, and EIP is 4 bytes above that.
    mov eax, esp
    mov ebx, [eax + 32]     ; error_code
    add eax, 36               ; eax -> EIP

    push eax                    ; 3rd arg: frame
    push ebx                      ; 2nd arg: error_code
    push dword %1                   ; 1st arg: vector
    call exception_handler
    add esp, 12

    popa

    ; Remove CPU-pushed error code.
    add esp, 4
    iret
%endmacro


ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

ISR_ERR 8

ISR_NOERR 9

ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14

ISR_NOERR 15
ISR_NOERR 16

ISR_ERR 17

ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31


; ------------------------------------------------
; Default unused vector
; ------------------------------------------------

isr_default:
    pusha

    mov eax, esp
    add eax, 32
    push eax
    push dword 0
    push dword 255
    call exception_handler
    add esp, 12

    popa
    iret


; ------------------------------------------------
; Timer IRQ0
; ------------------------------------------------

irq0:
    pusha

    mov eax, esp
    push eax
    call scheduler_irq
    add esp, 4

    mov esp, eax

    popa
    iret


; ------------------------------------------------
; Keyboard IRQ1
; ------------------------------------------------

irq1:
    pusha

    call kbd_handler

    popa
    iret


; ------------------------------------------------
; Remaining hardware IRQs
; ------------------------------------------------

%macro IRQ_DEFAULT 1
irq%1:
    pusha

    push dword %1
    call irq_unhandled_handler
    add esp, 4

    popa
    iret
%endmacro

IRQ_DEFAULT 2
IRQ_DEFAULT 3
IRQ_DEFAULT 4
IRQ_DEFAULT 5
IRQ_DEFAULT 6
IRQ_DEFAULT 7
IRQ_DEFAULT 8
IRQ_DEFAULT 9
IRQ_DEFAULT 10
IRQ_DEFAULT 11
IRQ_DEFAULT 12
IRQ_DEFAULT 13
IRQ_DEFAULT 14
IRQ_DEFAULT 15


; ------------------------------------------------
; System call
; ------------------------------------------------

isr_syscall:
    pusha

    ; PUSHA stack layout:
    ; [esp+00] EDI
    ; [esp+04] ESI
    ; [esp+08] EBP
    ; [esp+12] original ESP
    ; [esp+16] EBX
    ; [esp+20] EDX
    ; [esp+24] ECX
    ; [esp+28] EAX

    mov eax, esp
    push eax
    call syscall_handler
    add esp, 4

    ; Check saved EAX for SYS_EXIT.
    ; SYS_EXIT = 3.
    cmp dword [esp + 28], 3
    jne .normal_syscall_return

    ; The current task has been marked TERMINATED.
    ;
    ; Do NOT pop this task's frame and iret.
    ; Instead select another runnable task.

    mov eax, esp
    push eax
    call scheduler_exit_current
    add esp, 4

    ; EAX contains the next task's saved interrupt frame.
    mov esp, eax
    popa
    iret

.normal_syscall_return:
    popa
    iret

