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
global isr13
global isr14
global irq0
global irq1

extern exception_handler
extern timer_handler
extern kbd_handler

idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

isr0:
    pusha
    push dword 0
    call exception_handler
    add esp, 4
    popa
    iret

isr1:
    pusha
    push dword 1
    call exception_handler
    add esp, 4
    popa
    iret

isr2:
    pusha
    push dword 2
    call exception_handler
    add esp, 4
    popa
    iret

isr3:
    pusha
    push dword 3
    call exception_handler
    add esp, 4
    popa
    iret

isr4:
    pusha
    push dword 4
    call exception_handler
    add esp, 4
    popa
    iret

isr5:
    pusha
    push dword 5
    call exception_handler
    add esp, 4
    popa
    iret

isr6:
    pusha
    push dword 6
    call exception_handler
    add esp, 4
    popa
    iret

isr7:
    pusha
    push dword 7
    call exception_handler
    add esp, 4
    popa
    iret

; Exceptions 8, 13, and 14 automatically push an error code.
; Remove that error code before returning with iret.

isr8:
    pusha
    push dword 8
    call exception_handler
    add esp, 4
    popa
    add esp, 4
    iret

isr13:
    pusha
    push dword 13
    call exception_handler
    add esp, 4
    popa
    add esp, 4
    iret

isr14:
    pusha
    push dword 14
    call exception_handler
    add esp, 4
    popa
    add esp, 4
    iret

irq0:
    pusha
    call timer_handler
    popa
    iret

irq1:
    pusha
    call kbd_handler
    popa
    iret
