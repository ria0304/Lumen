BITS 32

global idt_load
global isr0
global isr3
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

isr3:
    pusha
    push dword 3
    call exception_handler
    add esp, 4
    popa
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
