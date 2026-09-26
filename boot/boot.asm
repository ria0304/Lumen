BITS 16
ORG 0x7C00

CODE_SEG equ 0x08
DATA_SEG equ 0x10

start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl

    mov si, loading_msg
    call print

    ; Load kernel at physical 0x10000
    mov ax, 0x1000
    mov es, ax
    xor bx, bx

    mov ah, 0x02
    mov al, 56
    mov ch, 0x00
    mov cl, 0x02
    mov dh, 0x00
    mov dl, [boot_drive]

    int 0x13
    jc disk_error

    xor ax, ax
    mov ds, ax
    mov si, read_ok_msg
    call print

    ; Enable A20
    in al, 0x92
    or al, 00000010b
    out 0x92, al

    mov si, a20_ok_msg
    call print

    ; Load GDT
    cli
    lgdt [gdt_descriptor]

    mov si, gdt_ok_msg
    call print

    ; Show that we are immediately before protected mode
    mov si, pm_msg
    call print

    ; Enable protected mode
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax

    ; Far jump into protected mode
    jmp dword CODE_SEG:protected_mode


disk_error:
    mov si, disk_error_msg
    call print
    jmp halt


print:
.next:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    int 0x10

    jmp .next

.done:
    ret


halt:
    cli
    hlt
    jmp halt


BITS 32

protected_mode:

    ; Load data segment FIRST
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, 0x90000

    ; PROTECTED MODE TEST
    mov word [0xB8000], 0x0741
    mov word [0xB8002], 0x0742
    mov word [0xB8004], 0x0743

    ; Jump to kernel
    mov eax, 0x10000
    jmp dword CODE_SEG:0x10000


BITS 16

boot_drive db 0

loading_msg    db "Loading kernel...", 0
read_ok_msg    db " READ_OK", 0
a20_ok_msg     db " A20_OK", 0
gdt_ok_msg     db " GDT_OK", 0
pm_msg         db " PM_START", 0
disk_error_msg db " DISK_ERROR", 0


gdt_start:

gdt_null:
    dq 0

gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_user_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b
    db 11001111b
    db 0x00

gdt_user_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start


times 510-($-$$) db 0
dw 0xAA55
